// The mouse in the game's Flash menus. A menu page is a screen that keeps a grid of items and takes the pad's input
// codes; its script names the item clips. The cursor focuses the item under it through the screen's own
// SetCurrentIndex, the left button presses the page's accept on the pad and the right button its cancel.
// The focus changes in the pad's turn: the movie's ActionScript calls the native Tick, which dispatches the queued
// pad input. A script's goto run outside it loses its frame's stop(), leaving items lit.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/aspect.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "game/flash.h"
#include "game/menu_ports.h"
#include "game/options_menu.h"
#include "game/pause_tabs.h"
#include "xdk/hle.h"

bool CaseShown();  // prompts.cpp: the attache case shows its keys

using namespace kernel;
using namespace flash;

namespace {

// input listener manager: listeners by priority, the last that takes a port and a code gets it
constexpr uint32_t kListenerManager = 0x787060;
constexpr uint32_t kManagerFirst = 0x08;
constexpr uint32_t kManagerEnd = 0x0C;

// input listener
constexpr uint32_t kListenerCodes = 0x08;  // bit per input code
constexpr uint32_t kListenerPorts = 0x0C;  // bit per controller port
constexpr uint32_t kListenerHeld = 0x14;   // nonzero: what reaches it goes nowhere
constexpr uint32_t kListenerOwner = 0x18;
constexpr uint32_t kScreenListenerVtable = 0x6D2CF8;

// menu screen, the grid behind a Flash page
constexpr uint32_t kMenuScreenVtable = 0x6B1A90;
constexpr uint32_t kScreenListener = 0x6C;
constexpr uint32_t kScreenScript = 0x88;        // the page script's handler, +4 its clip's path
constexpr uint32_t kScreenInputHandler = 0xA4;  // text: a script function takes the codes instead of the grid
constexpr uint32_t kScreenRows = 0xB4;
constexpr uint32_t kScreenColumns = 0xB8;
constexpr uint32_t kScreenCount = 0xBC;
constexpr uint32_t kScreenFocus = 0xC8;
constexpr uint32_t kScreenAccept = 0xCC;  // input codes
constexpr uint32_t kScreenCancel = 0xD0;
constexpr uint32_t kScreenBlocked = 0xDC;
constexpr uint32_t kScreenActive = 0xE0;
constexpr uint32_t kScriptPath = 0x04;
constexpr uint32_t kSetCurrentIndex = 0x19A61;  // thiscall(index): the script deselects the old, selects the new

// input codes
constexpr uint32_t kCodeStart = 0;
constexpr uint32_t kCodeSelect = 1;
constexpr uint32_t kCodeDown = 5;
constexpr uint32_t kCodeAccept = 6;
constexpr uint32_t kCodeCancel = 7;

// Flash player
constexpr uint32_t kStageWidth = 0x7626E8;  // float: the stage is drawn across the 640x480 screen
constexpr uint32_t kStageHeight = 0x7626EC;
constexpr uint32_t kClipBox = 0x2FF5A3;  // thiscall(float[4] out): min x, min y, max x, max y in its parent

// frame item: a label is {type, text}
constexpr uint32_t kItemLabelText = 0x04;
constexpr uint32_t kItemFrameLabel = 2;

// the world dossier's mission row: item i is the dot of mission kMissionDots[i]
constexpr uint32_t kMissionDots[] = {0, 1, 2, 3, 14, 4, 15, 5, 6, 7, 8, 9, 10, 16, 11, 12, 13, 17};

// item clips by the names the pages' scripts give them, the number between the two parts
struct ItemName {
    const char* prefix;
    const char* suffix;
    uint32_t first = 0;
    std::span<const uint32_t> numbers = {};
};
const ItemName kItemNames[] = {
    {"object", ""},
    {"textitem", ""},
    {"textmsg", "_mc"},
    {"item", ""},
    {"upgradeBox_mc", ""},
    {"leveldot", "", 0, kMissionDots},
    {"levelHolder", ""},
    {"labelName", ""},
};

// The pause menu's tabs have no names: the page keeps one at each depth, blue or red as it is focused.
// Its page is told apart by the clips beside them
constexpr int kPauseTabDepths[] = {124, 85, 46, 7};
constexpr const char* kPauseMarks[] = {"options_mc", "upgrades_mc", "border_mc"};
// The dossier's tabs are named "tab1" to "tab4", the focused one "on", but its timeline gives some the wrong number.
// Each keeps its row, so they are taken top to bottom
constexpr const char* kTabPrefix = "tab";
constexpr uint32_t kMatrixY = 5;
// the focused item keeps the cursor until another is this much nearer its middle: items move as focus does
constexpr float kStickiness = 0.25f;
// an item this much larger than the page's usual item on both sides is a focus glow
constexpr float kGlowScale = 1.5f;
// the frame labels a page's script puts its focused item on
constexpr const char* kFocusLabels[] = {"active", "default"};

constexpr int kSearchDepth = 3;
constexpr size_t kMaxSearch = 256;
constexpr uint32_t kMaxItems = 64;
constexpr uint32_t kMaxFrameItems = 256;
constexpr uint32_t kMaxFrames = 4096;
constexpr int kMaxParents = 32;
constexpr uint64_t kMenuGraceMs = 300;  // the cursor stays up between one page and the next

struct Point {
    float x;
    float y;
};

struct Box {
    float left;
    float top;
    float right;
    float bottom;
};

struct Extent {
    float half_w;
    float half_h;
};

// the item clips of a page in index order, as their names give it
struct Items {
    std::vector<uint32_t> clips;
};

enum class Order { Unknown, Forward, Reverse };

// what the cursor did since the last tick
struct Edges {
    bool moved;
    bool click;
    bool back;
};

// the focus the cursor asks for, applied in the pad's turn
struct Pending {
    uint32_t screen = 0;
    std::optional<uint32_t> index;
    bool accept = false;
};

struct CursorState {
    uint32_t screen = 0;
    Point seen{};
    bool left = false;
    bool right = false;
    uint32_t script = 0;
    Order order = Order::Unknown;  // of the script's items, once their labels showed it
    uint64_t menu_ms = 0;
    Pending pending;
    // each item's box while it is not focused: a focused rules row grows over the rows above it
    uint32_t boxes_screen = 0;
    uint32_t focused_clip = 0;
    std::map<uint32_t, Box> resting;
};
CursorState g_state;

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

// the screen that gets the keyboard port's input code, when one would get it
uint32_t ReceiverOf(uint32_t code) {
    uint32_t manager = R32(kListenerManager);
    int keyboard = input::KeyboardPort();
    if (!manager || keyboard < 0) return 0;
    uint32_t first = R32(manager + kManagerFirst);
    uint32_t at = R32(manager + kManagerEnd);
    for (; first && at > first; at -= 4) {
        uint32_t listener = R32(at - 4);
        bool port = R32(listener + kListenerPorts) >> keyboard & 1;
        bool takes = R32(listener + kListenerCodes) >> code & 1;
        if (!port || !takes) continue;
        if (R32(listener) != kScreenListenerVtable) return 0;
        return R32(listener + kListenerOwner);
    }
    return 0;
}

// the active menu page that the keyboard's confirm, down or start would reach
uint32_t MenuScreen() {
    for (uint32_t code : {kCodeAccept, kCodeDown, kCodeStart}) {
        uint32_t screen = ReceiverOf(code);
        if (screen && R32(screen) == kMenuScreenVtable && R32(screen + kScreenActive)) return screen;
    }
    return 0;
}

bool TakesInput(uint32_t screen) {
    return !R32(screen + kScreenBlocked) && !R32(screen + kScreenListener + kListenerHeld);
}

bool HasInputHandler(uint32_t screen) {
    uint32_t text = R32(screen + kScreenInputHandler);
    return Readable(text) && *Text(text);
}

input::Tap TapFor(uint32_t code, input::Tap fallback) {
    switch (code) {
        case kCodeStart:
            return input::Tap::Start;
        case kCodeSelect:
            return input::Tap::Back;
        case kCodeAccept:
            return input::Tap::A;
        case kCodeCancel:
            return input::Tap::B;
    }
    return fallback;
}

// clips

// a target path as the player writes one: "_level2.text_mc", or "/text_mc" on level 0
uint32_t Resolve(const std::string& path) {
    constexpr const char* kLevel = "_level";
    constexpr const char* kSeparators = "./";
    size_t start = path.find_first_not_of(kSeparators);
    uint32_t clip = Level(0);
    if (start != std::string::npos && path.compare(start, std::strlen(kLevel), kLevel) == 0) {
        clip = Level(std::atoi(path.c_str() + start + std::strlen(kLevel)));
        start = path.find_first_of(kSeparators, start);
    }
    while (clip && start != std::string::npos) {
        start = path.find_first_not_of(kSeparators, start);
        if (start == std::string::npos) break;
        size_t end = path.find_first_of(kSeparators, start);
        clip = Named(Children(clip), path.substr(start, end - start).c_str());
        start = end;
    }
    return clip;
}

// the last frame label at or before the sprite's frame
const char* LabelIn(uint32_t frame) {
    uint32_t count = std::min(R32(frame), kMaxFrameItems);
    uint32_t items = R32(frame + kFrameItems);
    if (!Readable(items)) return nullptr;
    const char* label = nullptr;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t item = R32(items + i * 4);
        if (!Readable(item) || R32(item) != kItemFrameLabel) continue;
        uint32_t text = R32(item + kItemLabelText);
        if (Readable(text)) label = Text(text);
    }
    return label;
}

const char* FrameLabel(uint32_t clip) {
    uint32_t character = Character(clip);
    if (!character) return nullptr;
    uint32_t frame = R32(R32(clip + kClipData) + kSpriteFrame);
    uint32_t frames = R32(character + kCharacterFrames);
    uint32_t last = std::min({frame + 1, R32(character + kCharacterFrameCount), kMaxFrames});
    const char* label = nullptr;
    for (uint32_t i = 0; Readable(frames) && i < last; i++) {
        if (const char* found = LabelIn(frames + i * kFrameSize)) label = found;
    }
    return label;
}

bool ShowsFocus(uint32_t clip) {
    const char* label = FrameLabel(clip);
    if (!label) return false;
    return std::any_of(std::begin(kFocusLabels), std::end(kFocusLabels),
                       [label](const char* focus) { return _stricmp(label, focus) == 0; });
}

// items

uint32_t NumberOf(const ItemName& name, uint32_t i) {
    if (name.numbers.empty()) return name.first + i;
    return i < name.numbers.size() ? name.numbers[i] : UINT_MAX;
}

uint32_t NamedItem(const std::vector<uint32_t>& children, const ItemName& name, uint32_t i) {
    std::string stem = name.prefix + std::to_string(NumberOf(name, i));
    return Named(children, (stem + name.suffix).c_str());
}

std::optional<Items> NamedItems(const std::vector<uint32_t>& children, uint32_t count) {
    for (const ItemName& name : kItemNames) {
        std::vector<uint32_t> clips;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t clip = NamedItem(children, name, i);
            if (!clip) break;
            clips.push_back(clip);
        }
        if (clips.size() == count) return Items{clips};
    }
    return std::nullopt;
}

bool IsPausePage(const std::vector<uint32_t>& children) {
    return std::all_of(std::begin(kPauseMarks), std::end(kPauseMarks),
                       [&](const char* mark) { return Named(children, mark) != 0; });
}

std::optional<Items> PauseTabs(const std::vector<uint32_t>& children, uint32_t count) {
    if (count != std::size(kPauseTabDepths) || !IsPausePage(children)) return std::nullopt;
    std::vector<uint32_t> clips;
    for (int depth : kPauseTabDepths) {
        auto at = std::find_if(children.begin(), children.end(), [depth](uint32_t c) { return Depth(c) == depth; });
        if (at == children.end()) return std::nullopt;
        clips.push_back(*at);
    }
    return Items{clips};
}

bool IsTab(const char* name) {
    size_t prefix = strlen(kTabPrefix);
    if (!name || strncmp(name, kTabPrefix, prefix) != 0 || !isdigit(uint8_t(name[prefix]))) return false;
    const char* rest = name + prefix + 1;
    return !*rest || strcmp(rest, "on") == 0;
}

std::optional<Items> DossierTabs(const std::vector<uint32_t>& children, uint32_t count) {
    std::vector<uint32_t> clips;
    std::copy_if(children.begin(), children.end(), std::back_inserter(clips), [](uint32_t c) { return IsTab(Name(c)); });
    if (clips.size() != count) return std::nullopt;
    auto row = [](uint32_t clip) { return (&G<float>(clip + kClipMatrix))[kMatrixY]; };
    std::sort(clips.begin(), clips.end(), [&](uint32_t a, uint32_t b) { return row(a) < row(b); });
    return Items{clips};
}

// The language page's items are its unnamed text fields, one a row of its single column, top to bottom
std::optional<Items> TextRows(const std::vector<uint32_t>& children, uint32_t count) {
    std::vector<uint32_t> clips;
    auto unnamed_text = [](uint32_t c) { return Type(c) == kTypeEditText && !*Name(c); };
    std::copy_if(children.begin(), children.end(), std::back_inserter(clips), unnamed_text);
    if (clips.size() != count) return std::nullopt;
    auto row = [](uint32_t clip) { return (&G<float>(clip + kClipMatrix))[kMatrixY]; };
    std::sort(clips.begin(), clips.end(), [&](uint32_t a, uint32_t b) { return row(a) < row(b); });
    return Items{clips};
}

// the item clips a page has
uint32_t ClipCount(uint32_t screen) {
    uint32_t count = R32(screen + kScreenCount);
    uint32_t stock = uint32_t(OptionsMenuStockRows(screen));
    return stock ? std::min(count, stock) : count;
}

// breadth first from the page's own clip
std::optional<Items> FindItems(uint32_t screen) {
    uint32_t script = R32(screen + kScreenScript);
    uint32_t count = ClipCount(screen);
    uint32_t path = script ? R32(script + kScriptPath) : 0;
    if (count == 0 || count > kMaxItems || !Readable(path)) return std::nullopt;
    std::vector<uint32_t> parents{Resolve(Text(path))};
    if (auto tabs = PauseTabs(Children(parents.front()), count)) return tabs;
    if (auto tabs = DossierTabs(Children(parents.front()), count)) return tabs;
    for (int depth = 0; depth < kSearchDepth && parents.front(); depth++) {
        std::vector<uint32_t> next;
        for (uint32_t parent : parents) {
            std::vector<uint32_t> children = Children(parent);
            if (auto items = NamedItems(children, count)) return items;
            next.insert(next.end(), children.begin(), children.end());
        }
        if (next.empty() || next.size() > kMaxSearch) break;
        parents = std::move(next);
    }
    bool one_column = R32(screen + kScreenColumns) == 1;
    return one_column ? TextRows(Children(Resolve(Text(path))), count) : std::nullopt;
}

// the pause menu's tab screen, not its options screen on the same page
bool IsPauseTabs(uint32_t screen) {
    uint32_t script = R32(screen + kScreenScript);
    uint32_t path = script ? R32(script + kScriptPath) : 0;
    if (!Readable(path) || OptionsMenuStockRows(screen)) return false;
    uint32_t page = Resolve(Text(path));
    return page && PauseTabs(Children(page), R32(screen + kScreenCount));
}

// which way the names count, from the one item the page shows focused
Order OrderOf(const std::vector<uint32_t>& clips, uint32_t focus) {
    int shown = -1;
    for (size_t i = 0; i < clips.size(); i++) {
        if (!ShowsFocus(clips[i])) continue;
        if (shown >= 0) return Order::Unknown;
        shown = int(i);
    }
    int last = int(clips.size()) - 1;
    bool forward = shown == int(focus);
    bool reverse = shown == last - int(focus);
    if (shown < 0 || forward == reverse) return Order::Unknown;
    return forward ? Order::Forward : Order::Reverse;
}

// items are taken in name order until their labels say otherwise
Order LatchOrder(uint32_t screen, const Items& items) {
    uint32_t script = R32(screen + kScreenScript);
    if (script != g_state.script) g_state.order = Order::Unknown;
    g_state.script = script;
    Order order = OrderOf(items.clips, R32(screen + kScreenFocus));
    if (order != Order::Unknown) g_state.order = order;
    if (g_state.order != Order::Unknown) return g_state.order;
    // forward until an item shows focus otherwise: a list may show none until its focus first moves
    return Order::Forward;
}

Point Transform(uint32_t clip, Point point) {
    const float* m = &G<float>(clip + kClipMatrix);
    return {m[0] * point.x + m[2] * point.y + m[4], m[1] * point.x + m[3] * point.y + m[5]};
}

// the clip's box on the stage; none when it or a parent is hidden, or it is empty
std::optional<Box> StageBox(X86Context& ctx, uint32_t clip) {
    static const uint32_t out = mem::AllocSystem(sizeof(Box));
    ctx.ecx = clip;
    CallGuest(ctx, kClipBox, {out});
    Box box = G<Box>(out);
    if (box.right < box.left || box.bottom < box.top) return std::nullopt;
    Point corners[] = {{box.left, box.top}, {box.right, box.top}, {box.left, box.bottom}, {box.right, box.bottom}};
    bool shown = Shown(clip);
    uint32_t parent = R32(clip + kClipParent);
    for (int i = 0; parent && i < kMaxParents; i++, parent = R32(parent + kClipParent)) {
        for (Point& corner : corners) corner = Transform(parent, corner);
        shown = shown && Shown(parent);
    }
    if (!shown) return std::nullopt;
    Box stage{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const Point& corner : corners) {
        stage = {std::min(stage.left, corner.x), std::min(stage.top, corner.y), std::max(stage.right, corner.x),
                 std::max(stage.bottom, corner.y)};
    }
    return stage;
}

// the point in the clip's own space, through every parent's matrix and its own undone
Point ToLocal(uint32_t clip, Point stage) {
    std::vector<uint32_t> chain;
    for (uint32_t at = clip; at && chain.size() < size_t(kMaxParents); at = R32(at + kClipParent)) chain.push_back(at);
    Point point = stage;
    for (auto at = chain.rbegin(); at != chain.rend(); ++at) {
        const float* m = &G<float>(*at + kClipMatrix);
        float det = m[0] * m[3] - m[1] * m[2];
        if (det == 0) return {NAN, NAN};
        float x = point.x - m[4];
        float y = point.y - m[5];
        point = {(m[3] * x - m[2] * y) / det, (m[0] * y - m[1] * x) / det};
    }
    return point;
}

// the clip's box in its own space: its matrix is made identity for the measure
Box LocalBox(X86Context& ctx, uint32_t clip) {
    static const uint32_t out = mem::AllocSystem(sizeof(Box));
    float* m = &G<float>(clip + kClipMatrix);
    float saved[6];
    std::copy(m, m + 6, saved);
    const float identity[6] = {1, 0, 0, 1, 0, 0};
    std::copy(identity, identity + 6, m);
    ctx.ecx = clip;
    CallGuest(ctx, kClipBox, {out});
    std::copy(saved, saved + 6, m);
    return G<Box>(out);
}

// the focused item keeps the box it had before it took focus; the others' boxes are kept for when they take it
Box LatchItemBox(X86Context& ctx, uint32_t clip) {
    Box live = LocalBox(ctx, clip);
    auto rest = g_state.resting.find(clip);
    if (clip != g_state.focused_clip) {
        g_state.resting[clip] = live;
        return live;
    }
    return rest != g_state.resting.end() ? rest->second : live;
}

void LatchFocusedClip(uint32_t screen, uint32_t focused_clip) {
    if (screen != g_state.boxes_screen) g_state.resting.clear();
    g_state.boxes_screen = screen;
    g_state.focused_clip = focused_clip;
}

Extent ExtentOf(const Box& box) { return {(box.right - box.left) / 2, (box.bottom - box.top) / 2}; }

float Median(std::vector<float> values) {
    auto middle = values.begin() + (values.size() - 1) / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

// the page's usual item size in the items' own space, each side's median
Extent UsualExtent(X86Context& ctx, const std::vector<uint32_t>& clips) {
    std::vector<float> widths;
    std::vector<float> heights;
    for (uint32_t clip : clips) {
        Extent extent = ExtentOf(LatchItemBox(ctx, clip));
        widths.push_back(extent.half_w);
        heights.push_back(extent.half_h);
    }
    if (clips.empty()) return {};
    return {Median(widths), Median(heights)};
}

// How far inside the item's own box the point is: 0 at its middle, 1 on its edge, none outside or hidden. A rotated
// or scaled item is tested in its own space, so its box does not swell into its neighbours'. A focused item's glow
// is left out: it is measured at the usual size around its middle
std::optional<float> Nearness(X86Context& ctx, uint32_t clip, Point stage, Extent usual) {
    if (!StageBox(ctx, clip)) return std::nullopt;
    Box box = LatchItemBox(ctx, clip);
    Point point = ToLocal(clip, stage);
    Extent extent = ExtentOf(box);
    Point middle = {box.left + extent.half_w, box.top + extent.half_h};
    bool glow = extent.half_w > usual.half_w * kGlowScale && extent.half_h > usual.half_h * kGlowScale;
    if (glow) extent = usual;
    if (extent.half_w <= 0 || extent.half_h <= 0) return std::nullopt;
    float dx = std::fabs(point.x - middle.x) / extent.half_w;
    float dy = std::fabs(point.y - middle.y) / extent.half_h;
    float edge = std::max(dx, dy);
    if (!(edge <= 1)) return std::nullopt;
    return edge;
}

// The item under the point nearest its own middle. Focused items are drawn on top, so the top one is not the one
// meant
std::optional<uint32_t> ItemAt(X86Context& ctx, const std::vector<uint32_t>& clips, Point point, Extent usual) {
    std::optional<uint32_t> hit;
    float best = 2;
    for (uint32_t i = 0; i < clips.size(); i++) {
        std::optional<float> near = Nearness(ctx, clips[i], point, usual);
        if (!near || *near >= best) continue;
        best = *near;
        hit = i;
    }
    return hit;
}

// the recomp's tabs below the last stock one, spaced as the stock ones: their index past the clips
std::optional<uint32_t> ExtraTabAt(X86Context& ctx, const std::vector<uint32_t>& clips, Point stage, Extent usual) {
    std::optional<Box> first = StageBox(ctx, clips.front());
    std::optional<Box> last = StageBox(ctx, clips.back());
    if (!first || !last) return std::nullopt;
    float step = (last->top + last->bottom - first->top - first->bottom) / 2 / float(clips.size() - 1);
    for (uint32_t tab = 1; tab <= kExtraTabs; tab++) {
        Point at{stage.x, stage.y - step * float(tab)};
        if (Nearness(ctx, clips.back(), at, usual)) return uint32_t(clips.size()) - 1 + tab;
    }
    return std::nullopt;
}

// the cursor on the stage; on a wide frame hud.cpp keeps the menus 4:3 in the middle
Point StagePoint(const input::Pointer& cursor) {
    float x = cursor.x;
    if (game::Wide()) {
        float narrow = game::Narrow();
        x = (x - (1 - narrow) / 2) / narrow;
    }
    return {x * G<float>(kStageWidth), cursor.y * G<float>(kStageHeight)};
}

// TEMPORARY: item boxes as the cursor tests them
void TraceItems(X86Context& ctx, uint32_t screen, const Items& items, Point stage, std::optional<uint32_t> hit) {
    static uint32_t traced = 0;
    static std::optional<uint32_t> last_hit;
    if (screen != traced) {
        traced = screen;
        for (size_t i = 0; i < items.clips.size(); i++) {
            uint32_t clip = items.clips[i];
            std::optional<Box> box = StageBox(ctx, clip);
            Box local = LocalBox(ctx, clip);
            const char* label = FrameLabel(clip);
            LOGR("menu trace: item %zu %s label %s stage %.1f %.1f %.1f %.1f local %.1f %.1f %.1f %.1f", i,
                 Name(clip), label ? label : "-", box ? box->left : NAN, box ? box->top : NAN, box ? box->right : NAN,
                 box ? box->bottom : NAN, local.left, local.top, local.right, local.bottom);
        }
    }
    if (hit == last_hit) return;
    last_hit = hit;
    LOGR("menu trace: cursor %.1f %.1f hit %d focus %u", stage.x, stage.y, hit ? int(*hit) : -1,
         R32(screen + kScreenFocus));
}

// acting

input::PageKeys KeysOf(uint32_t screen) {
    input::Tap accept = TapFor(R32(screen + kScreenAccept), input::Tap::A);
    return {accept, TapFor(R32(screen + kScreenCancel), input::Tap::B)};
}

// one jump to the item, then the click's accept, which the pad reads after it
void ApplyPending(X86Context& ctx) {
    Pending pending = g_state.pending;
    g_state.pending = {};
    if (!pending.index || pending.screen != MenuScreen()) return;
    if (*pending.index != R32(pending.screen + kScreenFocus)) {
        ctx.ecx = pending.screen;
        CallGuest(ctx, kSetCurrentIndex, {*pending.index});
    }
    if (pending.accept) input::Press(KeysOf(pending.screen).accept);
}

// the item the cursor is on, pressed by a click or by a click on it waiting for its jump
void PointAt(uint32_t screen, uint32_t index, Edges edges) {
    Pending& pending = g_state.pending;
    bool clicked = pending.accept && pending.screen == screen && pending.index == index;
    pending = {screen, index, edges.click || clicked};
}

void ApplyPointer(X86Context& ctx, uint32_t screen, Point stage, Edges edges) {
    // a recomp page's lines are its own, not the stock items
    if (std::optional<int> row = OptionsMenuRowAt(screen, stage.x, stage.y)) {
        if (*row >= 0) PointAt(screen, uint32_t(*row), edges);
        return;
    }
    input::Tap accept = KeysOf(screen).accept;
    std::optional<Items> items = HasInputHandler(screen) ? std::nullopt : FindItems(screen);
    // a page with nothing to point at takes a click anywhere
    if (!items) {
        if (edges.click) input::Press(accept);
        return;
    }
    Order order = LatchOrder(screen, *items);
    uint32_t last = uint32_t(items->clips.size()) - 1;
    uint32_t focus = R32(screen + kScreenFocus);
    uint32_t focused = order == Order::Reverse ? last - focus : focus;
    LatchFocusedClip(screen, focused <= last ? items->clips[focused] : 0);
    Extent usual = UsualExtent(ctx, items->clips);
    std::optional<uint32_t> hit = ItemAt(ctx, items->clips, stage, usual);
    TraceItems(ctx, screen, *items, stage, hit);
    bool tabs = IsPauseTabs(screen);
    if (!hit && tabs) hit = ExtraTabAt(ctx, items->clips, stage, usual);
    if (!hit) return;
    bool extra = *hit > last;
    bool options = OptionsMenuStockRows(screen) > 0;
    if (!options && !extra && focused <= last && focused != *hit) {
        std::optional<float> keep = Nearness(ctx, items->clips[focused], stage, usual);
        std::optional<float> near = Nearness(ctx, items->clips[*hit], stage, usual);
        if (keep && *keep < *near + kStickiness) hit = focused;
    }
    // a recomp tab keeps the stock screen on its last tab
    int tab = int(*hit);
    if (extra) hit = last;
    uint32_t slot = order == Order::Reverse ? last - *hit : *hit;
    if (tabs) PointPauseTab(extra ? tab : int(slot));
    uint32_t index = options ? uint32_t(OptionsMenuIndexAt(int(slot))) : slot;
    PointAt(screen, index, edges);
}

// a new page starts from where the cursor rests: only a move or a click after that acts
Edges LatchEdges(uint32_t screen, const input::Pointer& cursor) {
    bool same_page = screen == g_state.screen;
    bool moved = same_page && (cursor.x != g_state.seen.x || cursor.y != g_state.seen.y);
    Edges edges{moved, cursor.left && !g_state.left, cursor.right && !g_state.right};
    g_state.screen = screen;
    g_state.seen = {cursor.x, cursor.y};
    g_state.left = cursor.left;
    g_state.right = cursor.right;
    return edges;
}

// TEMPORARY: logs each menu page the cursor meets and its clip tree
constexpr bool kTraceMenus = true;

void TraceClips(uint32_t clip, int level) {
    std::vector<uint32_t> children = Children(clip);
    for (uint32_t child : children) {
        const char* label = FrameLabel(child);
        LOGR("menu trace: %*s%s type %u depth %d shown %d label %s", level * 2, "", Name(child), Type(child),
             Depth(child), Shown(child) ? 1 : 0, label ? label : "-");
        if (level < kSearchDepth) TraceClips(child, level + 1);
    }
}

void TraceKeyboardListeners() {
    uint32_t manager = R32(kListenerManager);
    int keyboard = input::KeyboardPort();
    LOGR("menu trace: no menu screen, keyboard port %d", keyboard);
    uint32_t first = manager ? R32(manager + kManagerFirst) : 0;
    uint32_t end = manager ? R32(manager + kManagerEnd) : 0;
    for (uint32_t at = first; first && at < end; at += 4) {
        uint32_t listener = R32(at);
        uint32_t owner = R32(listener + kListenerOwner);
        LOGR("menu trace: listener %08X vtable %08X codes %08X ports %08X held %u owner %08X owner vtable %08X",
             listener, R32(listener), R32(listener + kListenerCodes), R32(listener + kListenerPorts),
             R32(listener + kListenerHeld), owner, Readable(owner) ? R32(owner) : 0);
    }
}

void TraceScreen(uint32_t screen) {
    static uint32_t traced = UINT32_MAX;
    if (!kTraceMenus || screen == traced) return;
    traced = screen;
    if (!screen) return TraceKeyboardListeners();
    uint32_t script = R32(screen + kScreenScript);
    uint32_t path = script ? R32(script + kScriptPath) : 0;
    uint32_t handler = R32(screen + kScreenInputHandler);
    const char* path_text = Readable(path) ? Text(path) : "-";
    const char* handler_text = Readable(handler) ? Text(handler) : "-";
    std::optional<Items> items = FindItems(screen);
    LOGR("menu trace: screen %08X path %s rows %u columns %u count %u focus %u accept %d cancel %d handler %s items %d",
         screen, path_text, R32(screen + kScreenRows), R32(screen + kScreenColumns), R32(screen + kScreenCount),
         R32(screen + kScreenFocus), int(R32(screen + kScreenAccept)), int(R32(screen + kScreenCancel)), handler_text,
         items ? int(items->clips.size()) : -1);
    uint32_t page = Readable(path) ? Resolve(Text(path)) : 0;
    if (page) TraceClips(page, 0);
}

void UpdateMenu(uint32_t screen) {
    uint64_t now = NowMs();
    if (screen) g_state.menu_ms = now;
    input::SetMenu(screen || now - g_state.menu_ms < kMenuGraceMs);
}

void UpdateCursor(X86Context& ctx) {
    // the attache case takes the bound keys and mouse buttons as play does
    uint32_t screen = CaseShown() ? 0 : MenuScreen();
    TraceScreen(screen);
    UpdateMenu(screen);
    bool row = screen && R32(screen + kScreenRows) == 1 && R32(screen + kScreenColumns) > 1;
    input::SetMenuWheelAcross(row);
    UpdatePauseTabs(screen && IsPauseTabs(screen) ? screen : 0);
    input::SetPageKeys(screen ? std::optional(KeysOf(screen)) : std::nullopt);
    std::optional<input::Pointer> cursor = screen ? input::Cursor() : std::nullopt;
    if (!cursor) return;
    Edges edges = LatchEdges(screen, *cursor);
    // a key binding waits for the next press, mouse buttons too
    if (app::Capturing() || !TakesInput(screen)) return;
    if (edges.back) input::Press(KeysOf(screen).cancel);
    if (edges.moved || edges.click) ApplyPointer(ctx, screen, StagePoint(*cursor), edges);
}

}  // namespace

std::vector<MenuPorts> ActiveMenuPorts() {
    std::vector<MenuPorts> menus;
    uint32_t manager = R32(kListenerManager);
    uint32_t first = manager ? R32(manager + kManagerFirst) : 0;
    uint32_t end = manager ? R32(manager + kManagerEnd) : 0;
    for (uint32_t at = first; first && at < end; at += 4) {
        uint32_t listener = R32(at);
        if (R32(listener) != kScreenListenerVtable) continue;
        uint32_t screen = R32(listener + kListenerOwner);
        if (R32(screen) != kMenuScreenVtable) continue;
        uint32_t script = R32(screen + kScreenScript);
        uint32_t path = script ? R32(script + kScriptPath) : 0;
        uint32_t clip = Readable(path) ? Resolve(Text(path)) : 0;
        if (clip) menus.push_back({clip, R32(listener + kListenerPorts)});
    }
    return menus;
}

// a menu page with a grid of items takes the input; a page whose script takes the codes itself has none
bool MenuItemsShown() {
    uint32_t screen = MenuScreen();
    return screen && R32(screen + kScreenCount) && !HasInputHandler(screen);
}

// frame tick, after the pads are read and before the game updates
void MenuCursor(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    UpdateCursor(ctx);
    ctx = saved;
}

// the input manager's turn inside the movie's Tick, before the queued pad input is dispatched
void MenuCursorFocus(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    ApplyPending(ctx);
    PauseTabsFocus(ctx);
    OptionsMenuFocus(ctx);
    ctx = saved;
}
