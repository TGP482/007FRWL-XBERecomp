// The mouse in the game's Flash menus. A menu page is a screen that keeps a grid of items and takes the pad's input
// codes; its script names the item clips. The cursor focuses the item under it through the screen's own
// SetCurrentIndex, the left button presses the page's accept on the pad and the right button its cancel.
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/aspect.h"
#include "common/mouse_keyboard.h"
#include "xdk/hle.h"

using namespace kernel;

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
constexpr uint32_t kKeyboardPort = 0;

// Flash player
constexpr uint32_t kPlayer = 0x812560;
constexpr uint32_t kPlayerLevels = 0x28;
constexpr uint32_t kStageWidth = 0x7626E8;  // float: the stage is drawn across the 640x480 screen
constexpr uint32_t kStageHeight = 0x7626EC;
constexpr uint32_t kClipBox = 0x2FF5A3;  // thiscall(float[4] out): min x, min y, max x, max y in its parent

// clip
constexpr uint32_t kClipFlags = 0x04;   // type from bit 25, bit 4 set while it lives
constexpr uint32_t kClipName = 0x08;    // string
constexpr uint32_t kClipMatrix = 0x0C;  // a b c d tx ty
constexpr uint32_t kClipParent = 0x44;
constexpr uint32_t kClipData = 0x48;
constexpr uint32_t kClipNext = 0x50;
constexpr uint32_t kClipDepth = 0x54;  // low 17 bits, signed
constexpr uint32_t kClipVisible = 0x58;
constexpr uint32_t kVisibleBit = 0x100000;
constexpr uint32_t kLiveBit = 0x10;
constexpr uint32_t kTypeShift = 25;
constexpr uint32_t kTypeSprite = 0x0D;
constexpr uint32_t kTypeMovie = 0x12;
constexpr uint32_t kStringText = 0x08;
constexpr uint32_t kNoData = 0xBAADF00D;

// sprite data
constexpr uint32_t kSpriteCharacter = 0x0C;
constexpr uint32_t kSpriteFrame = 0x18;
constexpr uint32_t kSpriteChildren = 0x24;  // list: its head's next is the first child

// sprite character: frames of {item count, items}, each item a pointer to {type, ...}
constexpr uint32_t kCharacterFrameCount = 0x08;
constexpr uint32_t kCharacterFrames = 0x0C;
constexpr uint32_t kFrameSize = 8;
constexpr uint32_t kFrameItems = 0x04;
constexpr uint32_t kItemLabelText = 0x04;
constexpr uint32_t kItemFrameLabel = 2;

// item clips by the names the pages' scripts give them, the index between the two parts
struct ItemName {
    const char* prefix;
    const char* suffix;
};
constexpr ItemName kItemNames[] = {
    {"object", ""}, {"textitem", ""}, {"textmsg", "_mc"}, {"item", ""}, {"upgradeBox_mc", ""},
};
// the frame labels a page's script puts its focused item on
constexpr const char* kFocusLabels[] = {"active", "default"};

constexpr int kSearchDepth = 3;
constexpr size_t kMaxSearch = 256;
constexpr size_t kMaxSiblings = 512;
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

// the item clips of a page in index order, as their names give it
struct Items {
    std::vector<uint32_t> clips;
    bool own;  // children of the page's own clip
};

enum class Order { Unknown, Forward, Reverse };

// what the cursor did since the last tick
struct Edges {
    bool moved;
    bool click;
    bool back;
};

struct CursorState {
    uint32_t screen = 0;
    Point seen{};
    bool left = false;
    bool right = false;
    uint32_t script = 0;
    Order order = Order::Unknown;  // of the script's items, once their labels showed it
    uint64_t menu_ms = 0;
};
CursorState g_state;

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count());
}

bool Readable(uint32_t va) { return va && va != kNoData && mem::IsCommitted(va); }

const char* Text(uint32_t va) { return static_cast<const char*>(HostPtr(va)); }

// the screen that gets port 0's input code, when one would get it
uint32_t ReceiverOf(uint32_t code) {
    uint32_t manager = R32(kListenerManager);
    if (!manager) return 0;
    uint32_t first = R32(manager + kManagerFirst);
    uint32_t at = R32(manager + kManagerEnd);
    for (; first && at > first; at -= 4) {
        uint32_t listener = R32(at - 4);
        bool port = R32(listener + kListenerPorts) >> kKeyboardPort & 1;
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

uint32_t Type(uint32_t clip) {
    uint32_t flags = R32(clip + kClipFlags);
    return flags & kLiveBit ? flags >> kTypeShift : 0;
}

int Depth(uint32_t clip) { return int32_t(R32(clip + kClipDepth) << 15) >> 15; }

const char* Name(uint32_t clip) { return Text(R32(clip + kClipName) + kStringText); }

uint32_t FirstChild(uint32_t clip) {
    uint32_t type = Type(clip);
    if (type != kTypeSprite && type != kTypeMovie) return 0;
    uint32_t data = R32(clip + kClipData);
    if (!Readable(data)) return 0;
    uint32_t list = R32(data + kSpriteChildren);
    uint32_t head = Readable(list) ? R32(list) : 0;
    return Readable(head) ? R32(head + kClipNext) : 0;
}

std::vector<uint32_t> Children(uint32_t clip) {
    std::vector<uint32_t> children;
    for (uint32_t child = FirstChild(clip); child && children.size() < kMaxSiblings; child = R32(child + kClipNext)) {
        children.push_back(child);
    }
    return children;
}

uint32_t Named(const std::vector<uint32_t>& clips, const std::string& name) {
    for (uint32_t clip : clips) {
        if (_stricmp(Name(clip), name.c_str()) == 0) return clip;
    }
    return 0;
}

uint32_t Level(int number) {
    uint32_t player = R32(kPlayer);
    uint32_t levels = player ? R32(player + kPlayerLevels) : 0;
    uint32_t clip = levels ? R32(levels) : 0;
    for (size_t i = 0; clip && i < kMaxSiblings; i++, clip = R32(clip + kClipNext)) {
        if (Depth(clip) == number) return clip;
    }
    return 0;
}

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
        clip = Named(Children(clip), path.substr(start, end - start));
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
    uint32_t data = Type(clip) == kTypeSprite ? R32(clip + kClipData) : 0;
    uint32_t character = Readable(data) ? R32(data + kSpriteCharacter) : 0;
    if (!Readable(character)) return nullptr;
    uint32_t frames = R32(character + kCharacterFrames);
    uint32_t last = std::min({R32(data + kSpriteFrame) + 1, R32(character + kCharacterFrameCount), kMaxFrames});
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

std::optional<std::vector<uint32_t>> NamedItems(const std::vector<uint32_t>& children, uint32_t count) {
    for (const ItemName& name : kItemNames) {
        std::vector<uint32_t> clips;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t clip = Named(children, name.prefix + std::to_string(i) + name.suffix);
            if (!clip) break;
            clips.push_back(clip);
        }
        if (clips.size() == count) return clips;
    }
    return std::nullopt;
}

// breadth first from the page's own clip
std::optional<Items> FindItems(uint32_t screen) {
    uint32_t script = R32(screen + kScreenScript);
    uint32_t count = R32(screen + kScreenCount);
    uint32_t path = script ? R32(script + kScriptPath) : 0;
    if (count == 0 || count > kMaxItems || !Readable(path)) return std::nullopt;
    std::vector<uint32_t> parents{Resolve(Text(path))};
    for (int depth = 0; depth < kSearchDepth && parents.front(); depth++) {
        std::vector<uint32_t> next;
        for (uint32_t parent : parents) {
            std::vector<uint32_t> children = Children(parent);
            if (auto clips = NamedItems(children, count)) return Items{*clips, depth == 0};
            next.insert(next.end(), children.begin(), children.end());
        }
        if (next.empty() || next.size() > kMaxSearch) break;
        parents = std::move(next);
    }
    return std::nullopt;
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

// the page's own items are taken in name order until their labels say otherwise; deeper ones need the labels
Order LatchOrder(uint32_t screen, const Items& items) {
    uint32_t script = R32(screen + kScreenScript);
    if (script != g_state.script) g_state.order = Order::Unknown;
    g_state.script = script;
    Order order = OrderOf(items.clips, R32(screen + kScreenFocus));
    if (order != Order::Unknown) g_state.order = order;
    if (g_state.order != Order::Unknown) return g_state.order;
    return items.own ? Order::Forward : Order::Unknown;
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
    bool shown = R32(clip + kClipVisible) & kVisibleBit;
    uint32_t parent = R32(clip + kClipParent);
    for (int i = 0; parent && i < kMaxParents; i++, parent = R32(parent + kClipParent)) {
        for (Point& corner : corners) corner = Transform(parent, corner);
        shown = shown && (R32(parent + kClipVisible) & kVisibleBit);
    }
    if (!shown) return std::nullopt;
    Box stage{corners[0].x, corners[0].y, corners[0].x, corners[0].y};
    for (const Point& corner : corners) {
        stage = {std::min(stage.left, corner.x), std::min(stage.top, corner.y), std::max(stage.right, corner.x),
                 std::max(stage.bottom, corner.y)};
    }
    return stage;
}

// the item under the point, the one drawn on top where they overlap
std::optional<uint32_t> ItemAt(X86Context& ctx, const std::vector<uint32_t>& clips, Point point) {
    std::optional<uint32_t> hit;
    int top = INT_MIN;
    for (uint32_t i = 0; i < clips.size(); i++) {
        std::optional<Box> box = StageBox(ctx, clips[i]);
        bool inside = box && point.x >= box->left && point.x <= box->right && point.y >= box->top &&
                      point.y <= box->bottom;
        if (!inside || Depth(clips[i]) <= top) continue;
        top = Depth(clips[i]);
        hit = i;
    }
    return hit;
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

// acting

void Focus(X86Context& ctx, uint32_t screen, uint32_t index) {
    if (index == R32(screen + kScreenFocus)) return;
    ctx.ecx = screen;
    CallGuest(ctx, kSetCurrentIndex, {index});
}

void ApplyPointer(X86Context& ctx, uint32_t screen, Point stage, Edges edges) {
    input::Tap accept = TapFor(R32(screen + kScreenAccept), input::Tap::A);
    std::optional<Items> items = HasInputHandler(screen) ? std::nullopt : FindItems(screen);
    // a page with nothing to point at takes a click anywhere
    if (!items) {
        if (edges.click) input::Press(accept);
        return;
    }
    Order order = LatchOrder(screen, *items);
    std::optional<uint32_t> hit = ItemAt(ctx, items->clips, stage);
    if (order == Order::Unknown || !hit) return;
    uint32_t last = uint32_t(items->clips.size()) - 1;
    Focus(ctx, screen, order == Order::Reverse ? last - *hit : *hit);
    if (edges.click) input::Press(accept);
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

void UpdateMenu(uint32_t screen) {
    uint64_t now = NowMs();
    if (screen) g_state.menu_ms = now;
    input::SetMenu(screen || now - g_state.menu_ms < kMenuGraceMs);
}

void UpdateCursor(X86Context& ctx) {
    uint32_t screen = MenuScreen();
    UpdateMenu(screen);
    std::optional<input::Pointer> cursor = screen ? input::Cursor() : std::nullopt;
    if (!cursor) return;
    Edges edges = LatchEdges(screen, *cursor);
    if (!TakesInput(screen)) return;
    if (edges.back) input::Press(TapFor(R32(screen + kScreenCancel), input::Tap::B));
    if (edges.moved || edges.click) ApplyPointer(ctx, screen, StagePoint(*cursor), edges);
}

}  // namespace

// frame tick, after the pads are read and before the game updates
void MenuCursor(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    UpdateCursor(ctx);
    ctx = saved;
}
