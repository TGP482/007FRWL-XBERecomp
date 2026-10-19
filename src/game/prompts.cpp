// Button prompts on keyboard and mouse. A menu's button legend is a Flash clip of the button's picture between two
// texts (the shadow and the text): with the keyboard or mouse used last the picture is hidden and the text leads with
// the key, "(ENTER) Select". The game draws every line of text through one call, where the legends are relabelled
// and a button the font draws as a picture ("Press[RDDown]to continue") is named by its key. The console's wording
// reads in PC terms: the Xbox's names always, START and the controller with the keyboard or mouse used last.
#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/mouse_keyboard.h"
#include "game/flash.h"
#include "game/menu_ports.h"
#include "xdk/hle.h"

bool MenuItemsShown();  // menu_cursor.cpp

using namespace kernel;
using namespace flash;

namespace {

// text line draw, thiscall(font, rect, scale, text, align, flags); rect is x, y, width, height
constexpr uint32_t kDrawTextArg = 0x10;  // the text, from esp at the entry
constexpr uint32_t kDrawRectArg = 0x08;
constexpr uint32_t kDrawScaleArg = 0x0C;
constexpr uint32_t kDrawAlignArg = 0x14;  // 0 left, 1 centre, 2 right
constexpr uint32_t kRectX = 0x00;
constexpr uint32_t kRectWidth = 0x08;
constexpr uint32_t kMaxText = 0x400;

// text measure, thiscall(wrap width, wrap height, scale, text, float* width, float* height), the same text args
constexpr uint32_t kMeasureText = 0x356F90;
constexpr float kNoWrap = 1.0e6f;

// clip box, thiscall(float[4] out): min x, min y, max x, max y in its parent
constexpr uint32_t kClipBox = 0x2FF5A3;
constexpr uint32_t kMatrixTx = kClipMatrix + 0x10;
constexpr uint32_t kMatrixTy = kClipMatrix + 0x14;

constexpr float kLegendGap = 10.0f;  // stage units between one relabelled legend and the next

// frame item: a placement is {type, flags, depth, character id}
constexpr uint32_t kItemPlace = 3;
constexpr uint32_t kPlaceFlags = 0x04;
constexpr uint32_t kPlaceCharacter = 0x0C;
constexpr uint32_t kPlacesCharacter = 0x02;

// pad buttons as input::ButtonKey numbers them
enum PadButton { kA, kB, kX, kY, kBlack, kWhite, kLeftTrigger, kRightTrigger, kUp, kDown, kLeft, kRight, kStart, kBack };

// one button, or two read as a pair ("UP/DOWN")
struct Buttons {
    int first;
    int second = -1;
};

// a legend by the characters its first frame places in order: the shadow, the picture and the text, or the two texts
// and then the picture. Character ids are the movie's own
struct Legend {
    uint32_t places[3];
    Buttons buttons;
};
constexpr Legend kLegends[] = {
    {{14, 12, 15}, {kB}},             // xBoxButtons backBtn
    {{20, 19, 21}, {kLeft, kRight}},  // changeBtn, the dpad
    {{26, 25, 27}, {kBack}},          // goBackBtn
    {{32, 31, 33}, {kStart}},         // returnBtn
    {{38, 37, 39}, {kY}},             // deleteBtn
    {{44, 43, 45}, {kA}},             // selectBtn
    {{218, 219, 220}, {kA}},          // the briefcase's equip legend
    {{29, 21, 30}, {kA}},             // buttonsfrontend continueBtn
};

// a pad button in a text, "[RDDown]", by the start of its name: other languages add their own letters ("[RSTopFR]")
struct Token {
    const char* name;
    Buttons buttons;
};
constexpr Token kTokens[] = {
    {"RDDown", {kA}},
    {"RDRight", {kB}},
    {"RDLeft", {kX}},
    {"RDUp", {kY}},
    {"RSBottom", {kBlack}},
    {"LSBottom", {kWhite}},
    {"LSTop", {kLeftTrigger}},
    {"RSTop", {kRightTrigger}},
    {"LDUpDown", {kUp, kDown}},
    {"LDLeftRight", {kLeft, kRight}},
};

// English wording, whole words, applied in order
struct Swap {
    std::string_view from;
    std::string_view to;
};
constexpr Swap kPcWording[] = {
    {"Xbox console", "PC"},
    {"Your Xbox", "Your PC"},
    {"Xbox hard disk", "hard disk"},
    {"free blocks", "free space"},
    {"more blocks", "more space"},
};
constexpr Swap kKeyboardWording[] = {
    {"START on any controller", "START"},
};
constexpr std::string_view kStartWord = "START";

constexpr int kMaxTreeDepth = 12;
constexpr size_t kMaxVisits = 4096;
constexpr uint32_t kMaxFrameItems = 64;

// a legend's text in the language, and its button
struct LegendText {
    std::string text;
    Buttons buttons;
};

// a legend clip on screen and its text clips, the shadow and the text
struct LegendClip {
    uint32_t clip;
    std::vector<uint32_t> texts;
    std::string label;
};

// game thread: the legends on screen this tick, and whether their pictures are hidden
struct LegendState {
    std::vector<LegendText> texts;
    std::vector<LegendClip> clips;
    bool hidden = false;
    size_t visits = 0;
};
LegendState g_legends;
std::map<uint32_t, float> g_moved;  // legend texts moved into their hidden picture's room, by their place before

// a legend text moved along its row: where it sat and where it was last put; the texts are moved, not the legend,
// because the menus' timelines place their legends again each frame
struct Placed {
    float base;
    float written;
};
std::map<uint32_t, Placed> g_placed;
std::map<std::string, float> g_label_share;  // a legend text's drawn width over its field's, by its own text
bool g_measuring = false;

// split screen views, indexed by player port
constexpr uint32_t kViewSet = 0x778E64;
constexpr uint32_t kViewCount = 0x50;  // 0 outside split screen
int g_hud_port = -1;  // the player port whose split screen HUD is being drawn, -1 outside one

// game thread, each tick: each menu page's clip and the ports its screen takes
std::map<uint32_t, uint32_t> g_menu_ports;
uint32_t g_drawn_clip = 0;  // the Apt clip being drawn
std::optional<uint32_t> g_drawn_ports;  // the ports of the menu whose text is being drawn

bool TakesKeyboard(uint32_t ports) {
    int keyboard = input::KeyboardPort();
    return keyboard >= 0 && (ports >> keyboard & 1);
}

void UpdateMenuPorts() {
    g_menu_ports.clear();
    for (const MenuPorts& menu : ActiveMenuPorts()) g_menu_ports[menu.clip] = menu.ports;
}

// the ports of the nearest menu page holding the clip; the pages' texts are clips made again each frame, so they
// are looked up through their parents as they are drawn
std::optional<uint32_t> PortsOf(uint32_t clip) {
    for (int depth = 0; clip && depth <= kMaxTreeDepth; depth++) {
        auto found = g_menu_ports.find(clip);
        if (found != g_menu_ports.end()) return found->second;
        clip = R32(clip + kClipParent);
    }
    return std::nullopt;
}

// a clip outside every menu keeps the keyboard's prompts
bool ClipTakesKeyboard(uint32_t clip) {
    std::optional<uint32_t> ports = PortsOf(clip);
    return !ports || TakesKeyboard(*ports);
}

// a menu text follows its menu's ports, a split screen HUD its view's player
bool KeyboardPrompts() {
    if (!input::KeyboardActive()) return false;
    if (g_drawn_ports) return TakesKeyboard(*g_drawn_ports);
    return g_hud_port < 0 || g_hud_port == input::KeyboardPort();
}

std::string PauseKey() {
    std::string name = input::ButtonKey(kStart);
    return name == "Escape" ? "ESC" : name;
}

// a menu reads Enter for confirm, the pause key for cancel and the arrows for the dpad
std::string MenuKey(int button) {
    switch (button) {
        case kA:
            return "ENTER";
        case kB:
        case kStart:
            return PauseKey();
        case kUp:
            return "UP";
        case kDown:
            return "DOWN";
        case kLeft:
            return "LEFT";
        case kRight:
            return "RIGHT";
    }
    return input::ButtonKey(button);
}

std::string KeyName(int button, bool menu) { return menu ? MenuKey(button) : input::ButtonKey(button); }

// "(KEY)", empty when a button has no key
std::string Label(Buttons buttons, bool menu) {
    std::string key = KeyName(buttons.first, menu);
    if (buttons.second >= 0) {
        std::string second = KeyName(buttons.second, menu);
        key = second.empty() ? std::string() : key + "/" + second;
    }
    return key.empty() ? key : "(" + key + ")";
}

// legends

// the characters a sprite's first frame places, in order
std::vector<uint32_t> FirstPlaces(uint32_t character) {
    std::vector<uint32_t> places;
    uint32_t frames = R32(character + kCharacterFrames);
    if (!R32(character + kCharacterFrameCount) || !Readable(frames)) return places;
    uint32_t count = std::min(R32(frames), kMaxFrameItems);
    uint32_t items = R32(frames + kFrameItems);
    for (uint32_t i = 0; Readable(items) && i < count; i++) {
        uint32_t item = R32(items + i * 4);
        bool place = Readable(item) && R32(item) == kItemPlace && (R32(item + kPlaceFlags) & kPlacesCharacter);
        if (place) places.push_back(R32(item + kPlaceCharacter));
    }
    return places;
}

const Legend* LegendOf(uint32_t clip) {
    uint32_t character = Character(clip);
    if (!character) return nullptr;
    std::vector<uint32_t> places = FirstPlaces(character);
    if (places.size() != std::size(kLegends[0].places)) return nullptr;
    for (const Legend& legend : kLegends) {
        if (std::equal(places.begin(), places.end(), std::begin(legend.places))) return &legend;
    }
    return nullptr;
}

std::string ShownText(uint32_t field) {
    uint32_t text = R32(field + kFieldShownText);
    if (!Readable(text) || !*Text(text)) text = R32(field + kFieldText);
    if (!Readable(text)) return {};
    return std::string(Text(text), strnlen(Text(text), kMaxText));
}

void SetShown(uint32_t clip, bool shown) {
    uint32_t flags = R32(clip + kClipVisible);
    W32(clip + kClipVisible, shown ? flags | kVisibleBit : flags & ~kVisibleBit);
}

struct Span {
    float left;
    float right;
};

Span SpanOf(X86Context& ctx, uint32_t clip) {
    static const uint32_t out = mem::AllocSystem(4 * sizeof(float));
    uint32_t ecx = ctx.ecx;
    ctx.ecx = clip;
    CallGuest(ctx, kClipBox, {out});
    ctx.ecx = ecx;
    return {G<float>(out), G<float>(out + 8)};
}

float LeftOf(X86Context& ctx, uint32_t clip) { return SpanOf(ctx, clip).left; }

// a text after the picture takes the picture's room while it is hidden, so the longer label keeps clear of the next
void UpdateTextPlace(X86Context& ctx, uint32_t text, uint32_t picture, bool keyboard) {
    float& x = G<float>(text + kMatrixTx);
    auto moved = g_moved.find(text);
    if (!keyboard) {
        if (moved != g_moved.end()) x = moved->second;
        return;
    }
    if (moved != g_moved.end() || !picture) return;
    float room = LeftOf(ctx, text) - LeftOf(ctx, picture);
    if (room <= 0.0f) return;
    g_moved[text] = x;
    x -= room;
}

// a legend of a menu another player drives keeps its picture, its texts back where they sat
void KeepPadLegend(X86Context& ctx, uint32_t clip, uint32_t picture) {
    if (picture) SetShown(picture, true);
    for (uint32_t child : Children(clip)) {
        if (!EditTextField(child)) continue;
        UpdateTextPlace(ctx, child, picture, false);
        auto placed = g_placed.find(child);
        if (placed == g_placed.end()) continue;
        float& x = G<float>(child + kMatrixTx);
        if (x == placed->second.written) x = placed->second.base;
        g_placed.erase(placed);
    }
}

// the picture goes while the keys are shown, the texts are kept to be relabelled as they are drawn
void ApplyLegend(X86Context& ctx, uint32_t clip, const Legend& legend, bool keyboard) {
    std::vector<uint32_t> children = Children(clip);
    uint32_t picture = 0;
    for (uint32_t child : children) {
        if (Type(child) == kTypeSprite) picture = child;
    }
    if (keyboard && !ClipTakesKeyboard(clip)) return KeepPadLegend(ctx, clip, picture);
    if (picture) SetShown(picture, !keyboard);
    for (uint32_t child : children) {
        uint32_t field = EditTextField(child);
        if (!field) continue;
        UpdateTextPlace(ctx, child, picture, keyboard);
        if (g_legends.clips.empty() || g_legends.clips.back().clip != clip) {
            g_legends.clips.push_back({clip, {}, ShownText(field)});
        }
        g_legends.clips.back().texts.push_back(child);
        if (keyboard) g_legends.texts.push_back({ShownText(field), legend.buttons});
    }
}

// how far the text is moved now
float ShiftOf(uint32_t text) {
    auto placed = g_placed.find(text);
    bool ours = placed != g_placed.end() && placed->second.written == G<float>(text + kMatrixTx);
    return ours ? placed->second.written - placed->second.base : 0.0f;
}

// the text's x where it sat; the shift is written over it
void PlaceText(uint32_t text, float shift) {
    float& x = G<float>(text + kMatrixTx);
    auto placed = g_placed.find(text);
    if (placed == g_placed.end() || placed->second.written != x) {
        placed = g_placed.insert_or_assign(text, Placed{x, x}).first;
    }
    x = placed->second.base + shift;
    placed->second.written = x;
}

void PlaceLegend(const LegendClip& legend, float shift) {
    for (uint32_t text : legend.texts) PlaceText(text, shift);
}

// texts gone from the screen are forgotten; with the pad the rest go back where they sat
void UpdatePlaced(bool keyboard) {
    std::map<uint32_t, Placed> kept;
    for (const LegendClip& legend : g_legends.clips) {
        for (uint32_t text : legend.texts) {
            auto placed = g_placed.find(text);
            if (placed == g_placed.end()) continue;
            float& x = G<float>(text + kMatrixTx);
            if (keyboard) kept.insert(*placed);
            else if (x == placed->second.written) x = placed->second.base;
        }
    }
    g_placed = std::move(kept);
}

struct RowItem {
    const LegendClip* legend;
    float left;  // the text's, in the row's units with the text where it sat
    float width;
};

std::optional<RowItem> RowItemOf(X86Context& ctx, const LegendClip& legend) {
    auto share = g_label_share.find(legend.label);
    if (share == g_label_share.end() || legend.texts.empty()) return std::nullopt;
    uint32_t text = legend.texts.front();
    Span field = SpanOf(ctx, text);
    float scale = G<float>(legend.clip + kClipMatrix);
    float left = G<float>(legend.clip + kMatrixTx) + (field.left - ShiftOf(text)) * scale;
    return RowItem{&legend, left, share->second * (field.right - field.left) * scale};
}

// legends of one parent are spread so each longer label ends before the next begins; the row grows on both sides
void LayOutRow(X86Context& ctx, const std::vector<LegendClip>& row) {
    std::vector<RowItem> items;
    for (const LegendClip& legend : row) {
        std::optional<RowItem> item = RowItemOf(ctx, legend);
        if (!item) return;
        items.push_back(*item);
    }
    std::sort(items.begin(), items.end(), [](const RowItem& a, const RowItem& b) { return a.left < b.left; });
    std::vector<float> shifts(items.size(), 0.0f);
    float right = items.front().left + items.front().width;
    for (size_t i = 1; i < items.size(); i++) {
        float left = std::max(items[i].left, right + kLegendGap);
        shifts[i] = left - items[i].left;
        right = left + items[i].width;
    }
    float centre = shifts.back() / 2;
    for (size_t i = 0; i < items.size(); i++) {
        float scale = G<float>(items[i].legend->clip + kClipMatrix);  // the texts move in the legend's units
        if (scale != 0) PlaceLegend(*items[i].legend, (shifts[i] - centre) / scale);
    }
}

// a row is the legends of one parent on one line
void LayOutLegends(X86Context& ctx) {
    std::map<std::pair<uint32_t, long>, std::vector<LegendClip>> rows;
    for (const LegendClip& legend : g_legends.clips) {
        long line = std::lround(G<float>(legend.clip + kMatrixTy));
        rows[{R32(legend.clip + kClipParent), line}].push_back(legend);
    }
    for (const auto& [parent, row] : rows) {
        if (row.size() > 1) LayOutRow(ctx, row);
    }
}

// unnamed clips have no name string
std::string NameOf(uint32_t clip) {
    uint32_t name = R32(clip + kClipName);
    return Readable(name) ? std::string(Name(clip)) : std::string("-");
}

// the text's width on one line, as the line draw measures it
float LineWidth(X86Context& ctx, uint32_t text, float scale) {
    static const uint32_t out = mem::AllocSystem(2 * sizeof(float));
    uint32_t no_wrap = std::bit_cast<uint32_t>(kNoWrap);
    g_measuring = true;
    CallGuest(ctx, kMeasureText, {no_wrap, no_wrap, std::bit_cast<uint32_t>(scale), text, out, out + 4});
    g_measuring = false;
    return G<float>(out);
}

// attache case: its buttons are pictures, "subButton" clips placing a frame (2) and the button's picture. On keyboard
// and mouse the picture goes and the key is written in its place

constexpr const char* kCaseButton = "subButton";
constexpr const char* kCaseTv = "attachedClip_mc";
constexpr uint32_t kTypeShape = 0x0C;
constexpr uint32_t kCasePicturePlace = 1;  // in the clip's first places
constexpr uint32_t kItemRemove = 4;
constexpr uint32_t kRemoveDepth = 0x04;
constexpr uint32_t kPlaceDepth = 0x08;
constexpr uint32_t kMaxCaseFrames = 64;

struct CasePicture {
    uint32_t character;
    int button;
};
// minibuttons exports bottom2, right2, left2, top2, L12, R12, read by the case script as codes 6, 7, 13, 12, 8, 9
constexpr CasePicture kCasePictures[] = {
    {29, kA}, {13, kB}, {23, kX}, {4, kY}, {26, kLeftTrigger}, {18, kRightTrigger},
};

// the text manager, thiscall
constexpr uint32_t kTextManager = 0x77F70C;
constexpr uint32_t kSetColour = 0x355E10;  // palette index
constexpr uint32_t kDrawText = 0x357920;   // page position {x, y}, scale, text, alignment
constexpr uint32_t kSetFont = 0x355D70;    // font id, false and no font when it is not loaded
constexpr uint32_t kFontId = 0x3A7E60;     // cdecl (name), the hash fonts are registered under
constexpr uint32_t kDinLarge = 0x6D2F98;   // "din_large", the menus' heavy font
constexpr uint32_t kNextLayer = 0x412BC0;  // cdecl (1), the depth the menus' next text draws at
constexpr uint32_t kManagerFont = 0x1C;
constexpr uint32_t kManagerGlyphScale = 0x4C;
constexpr uint32_t kManagerPageWidth = 0x60;
constexpr uint32_t kManagerPageHeight = 0x64;
constexpr uint32_t kFontHeight = 0x18;  // int
constexpr uint32_t kTextBlack = 0;  // palette indices
constexpr uint32_t kTextWhite = 12;
constexpr uint32_t kAlignCentre = 1;
constexpr float kKeyHeight = 0.5f;  // of the picture's height
constexpr float kKeyWidth = 0.9f;   // of the picture's width, at most
constexpr float kShadow = 1.0f / 480.0f;  // of the page height
constexpr float kRollSpan = 0.6f;  // of the picture's height, how far the keys roll
constexpr double kRollMs = 100.0;  // until a roll is timed: 3 blur frames at 30 fps
constexpr double kMaxRollMs = 1000.0;

// RenderWare's record of the texture on stage 0, which the game's own SetTexture calls leave behind
constexpr uint32_t kAppliedTexture = 0x810D8C;
constexpr uint32_t kNoTexture = 0xFFFFFFFF;

constexpr uint32_t kStageWidth = 0x7626E8;  // float
constexpr uint32_t kStageHeight = 0x7626EC;
constexpr int kMaxParents = 32;

struct Point {
    float x;
    float y;
};

struct CaseKey {
    Point middle;  // share of the stage
    float width;   // share of the stage
    float height;  // share of the stage
    std::string key;
    uint32_t clip;  // the button, drawn with it
};

// a shape's matrix, kept while the shape is drawn at no size: shapes draw whether shown or not
struct Collapsed {
    uint32_t shape;
    std::array<float, 4> axes;
};

// a button rolling between pictures, timed so the keys roll smoothly between the timeline's frames
struct Roll {
    std::string settled;  // the key it last settled on
    double start_ms = 0;
    double length_ms = kRollMs;  // the last roll's
    bool rolling = false;
};

struct CaseState {
    std::vector<uint32_t> hidden;
    std::vector<Collapsed> collapsed;
    std::vector<CaseKey> keys;
    std::map<uint32_t, Roll> rolls;  // by the button's clip
};
CaseState g_case;

const CasePicture* CaseButtonOf(uint32_t clip) {
    uint32_t character = Character(clip);
    if (!character || NameOf(clip) != kCaseButton) return nullptr;
    std::vector<uint32_t> places = FirstPlaces(character);
    if (places.size() <= kCasePicturePlace) return nullptr;
    for (const CasePicture& picture : kCasePictures) {
        if (picture.character == places[kCasePicturePlace]) return &picture;
    }
    return nullptr;
}

// whether the button's timeline shows its picture now; between pictures it plays a blur of others
bool ShowsCasePicture(uint32_t clip, uint32_t picture) {
    uint32_t character = Character(clip);
    uint32_t frames = R32(character + kCharacterFrames);
    uint32_t frame = R32(R32(clip + kClipData) + kSpriteFrame);
    uint32_t last = std::min({frame + 1, R32(character + kCharacterFrameCount), kMaxCaseFrames});
    std::map<uint32_t, uint32_t> shown;  // character by depth
    for (uint32_t i = 0; Readable(frames) && i < last; i++) {
        uint32_t count = std::min(R32(frames + i * kFrameSize), kMaxFrameItems);
        uint32_t items = R32(frames + i * kFrameSize + kFrameItems);
        for (uint32_t k = 0; Readable(items) && k < count; k++) {
            uint32_t item = R32(items + k * 4);
            if (!Readable(item)) continue;
            if (R32(item) == kItemRemove) shown.erase(R32(item + kRemoveDepth));
            bool place = R32(item) == kItemPlace && (R32(item + kPlaceFlags) & kPlacesCharacter);
            if (place) shown[R32(item + kPlaceDepth)] = R32(item + kPlaceCharacter);
        }
    }
    return std::any_of(shown.begin(), shown.end(), [&](const auto& depth) { return depth.second == picture; });
}

Point Transform(uint32_t clip, Point point) {
    const float* m = &G<float>(clip + kClipMatrix);
    return {m[0] * point.x + m[2] * point.y + m[4], m[1] * point.x + m[3] * point.y + m[5]};
}

// the clip's middle and height as shares of the stage; none when a parent is hidden
std::optional<CaseKey> CaseKeyOf(X86Context& ctx, uint32_t clip) {
    static const uint32_t out = mem::AllocSystem(4 * sizeof(float));
    uint32_t ecx = ctx.ecx;
    ctx.ecx = clip;
    CallGuest(ctx, kClipBox, {out});
    ctx.ecx = ecx;
    Point low = {G<float>(out), G<float>(out + 4)};
    Point high = {G<float>(out + 8), G<float>(out + 12)};
    uint32_t parent = R32(clip + kClipParent);
    for (int i = 0; parent && i < kMaxParents; i++, parent = R32(parent + kClipParent)) {
        if (!Shown(parent)) return std::nullopt;
        low = Transform(parent, low);
        high = Transform(parent, high);
    }
    float width = G<float>(kStageWidth);
    float height = G<float>(kStageHeight);
    if (!(width > 0) || !(height > 0) || high.y <= low.y) return std::nullopt;
    Point middle = {(low.x + high.x) / 2 / width, (low.y + high.y) / 2 / height};
    return CaseKey{middle, (high.x - low.x) / width, (high.y - low.y) / height, {}, clip};
}

void ShowCasePictures() {
    for (uint32_t clip : g_case.hidden) SetShown(clip, true);
    for (const Collapsed& picture : g_case.collapsed) {
        std::copy(picture.axes.begin(), picture.axes.end(), &G<float>(picture.shape + kClipMatrix));
    }
    g_case.hidden.clear();
    g_case.collapsed.clear();
}

void CollapseForRender(uint32_t shape) {
    float* axes = &G<float>(shape + kClipMatrix);
    Collapsed picture{shape, {}};
    std::copy(axes, axes + picture.axes.size(), picture.axes.begin());
    std::fill(axes, axes + picture.axes.size(), 0.0f);
    g_case.collapsed.push_back(picture);
}

// the button's picture, the shape it places beside its frame; an entered button's picture is removed
uint32_t CasePictureOf(uint32_t clip) {
    for (uint32_t child : Children(clip)) {
        if (Type(child) == kTypeShape) return child;
    }
    return 0;
}

bool InCaseTv(uint32_t clip) {
    uint32_t parent = R32(clip + kClipParent);
    return Readable(parent) && NameOf(parent) == kCaseTv;
}

void HideForRender(uint32_t clip) {
    SetShown(clip, false);
    g_case.hidden.push_back(clip);
}

void HideSprites(const std::vector<uint32_t>& clips, uint32_t keep) {
    for (uint32_t clip : clips) {
        if (clip != keep && Type(clip) == kTypeSprite && Shown(clip)) HideForRender(clip);
    }
}

// the button's frame is a sprite beside its picture
void HideCaseFrame(uint32_t clip) { HideSprites(Children(clip), 0); }

// a trigger's $TRIGGER_L or $TRIGGER_R text is a sprite beside the button, the other buttons have none
void HideTriggerText(uint32_t clip) { HideSprites(Children(R32(clip + kClipParent)), clip); }

void AddCaseKey(CaseKey place, const std::string& key, float shift) {
    place.middle.y += shift * place.height;
    place.key = key;
    g_case.keys.push_back(place);
}

double NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(since).count();
}

// the button rolls between pictures through a blur of others: the last key rolls up and out, this one in below
void AddRollingKeys(uint32_t clip, const CaseKey& place, const std::string& key) {
    Roll& roll = g_case.rolls[clip];
    if (!roll.rolling) roll.start_ms = NowMs();
    roll.rolling = true;
    float done = float(std::min((NowMs() - roll.start_ms) / roll.length_ms, 1.0));
    if (!roll.settled.empty()) AddCaseKey(place, roll.settled, -done * kRollSpan);
    AddCaseKey(place, key, (1.0f - done) * kRollSpan);
}

void AddSettledKey(uint32_t clip, const CaseKey& place, const std::string& key) {
    Roll& roll = g_case.rolls[clip];
    double length = NowMs() - roll.start_ms;
    if (roll.rolling && length > 0 && length < kMaxRollMs) roll.length_ms = length;
    roll.rolling = false;
    roll.settled = key;
    AddCaseKey(place, key, 0.0f);
}

void VisitCase(X86Context& ctx, uint32_t clip, int depth) {
    if (const CasePicture* button = CaseButtonOf(clip)) {
        if (!Shown(clip)) return;
        if (InCaseTv(clip)) HideCaseFrame(clip);
        HideTriggerText(clip);
        uint32_t picture = CasePictureOf(clip);
        std::string key = input::ButtonKey(button->button);
        std::optional<CaseKey> place = !picture || key.empty() ? std::nullopt : CaseKeyOf(ctx, clip);
        if (!place) return;
        CollapseForRender(picture);
        if (!ShowsCasePicture(clip, button->character)) return AddRollingKeys(clip, *place, key);
        AddSettledKey(clip, *place, key);
        return;
    }
    if (depth >= kMaxTreeDepth) return;
    for (uint32_t child : Children(clip)) VisitCase(ctx, child, depth + 1);
}

// the pictures are hidden for the render alone, so the game's own showing and hiding of them is untouched
void UpdateCaseKeys(X86Context& ctx) {
    g_case.keys.clear();
    g_case.hidden.clear();
    g_case.collapsed.clear();
    if (!input::KeyboardActive()) return;
    for (uint32_t level : Levels()) VisitCase(ctx, level, 0);
}

void DrawCaseKey(X86Context& ctx, uint32_t manager, const CaseKey& key) {
    static const uint32_t position = mem::AllocSystem(8 + kMaxText);
    uint32_t text = position + 8;
    std::copy(key.key.begin(), key.key.end(), &G<char>(text));
    G<char>(text + uint32_t(key.key.size())) = 0;
    float page_width = G<float>(manager + kManagerPageWidth);
    float page_height = G<float>(manager + kManagerPageHeight);
    uint32_t font = R32(manager + kManagerFont);
    float unit = float(int32_t(R32(font + kFontHeight))) * G<float>(manager + kManagerGlyphScale);
    if (!(unit > 0)) return;
    float scale = key.height * page_height * kKeyHeight / unit;
    ctx.ecx = manager;
    float drawn = LineWidth(ctx, text, scale);
    float room = key.width * page_width * kKeyWidth;
    if (drawn > room) scale *= room / drawn;
    float x = key.middle.x * page_width;
    float y = page_height * (1 - key.middle.y) - scale * unit / 2;  // the page counts y up, the text sits on y
    struct Pass {
        uint32_t colour;
        float dx;
        float dy;
    };
    float shadow = kShadow * page_height;
    for (Pass pass : {Pass{kTextBlack, shadow, -shadow}, Pass{kTextWhite, 0, 0}}) {
        G<float>(position) = x + pass.dx;
        G<float>(position + 4) = y + pass.dy;
        ctx.ecx = manager;
        CallGuest(ctx, kSetColour, {pass.colour});
        ctx.ecx = manager;
        CallGuest(ctx, kDrawText, {position, std::bit_cast<uint32_t>(scale), text, kAlignCentre});
    }
}

void Visit(X86Context& ctx, uint32_t clip, int depth, bool keyboard) {
    if (++g_legends.visits > kMaxVisits) return;
    if (const Legend* legend = LegendOf(clip)) return ApplyLegend(ctx, clip, *legend, keyboard);
    if (depth >= kMaxTreeDepth) return;
    for (uint32_t child : Children(clip)) Visit(ctx, child, depth + 1, keyboard);
}

void UpdateLegends(X86Context& ctx) {
    bool keyboard = input::KeyboardActive();
    g_legends.texts.clear();
    g_legends.clips.clear();
    if (!keyboard && !g_legends.hidden) return;
    g_legends.visits = 0;
    for (uint32_t level : Levels()) Visit(ctx, level, 0, keyboard);
    UpdatePlaced(keyboard);
    if (keyboard) LayOutLegends(ctx);
    if (!keyboard) g_moved.clear();
    g_legends.hidden = keyboard;
}

// rewording

bool LeadWithKey(std::string& text) {
    for (const LegendText& legend : g_legends.texts) {
        if (legend.text != text) continue;
        std::string label = Label(legend.buttons, true);
        if (label.empty()) return false;
        text = label + " " + text;
        return true;
    }
    return false;
}

const Token* TokenNamed(const std::string& name) {
    for (const Token& token : kTokens) {
        if (name.compare(0, std::strlen(token.name), token.name) == 0) return &token;
    }
    return nullptr;
}

bool SpaceAt(const std::string& text, size_t at) {
    return at >= text.size() || std::isspace(static_cast<unsigned char>(text[at]));
}

// the token's picture had room around it; the key is set apart by spaces
bool NameKeys(std::string& text) {
    bool menu = input::CursorShown();
    bool changed = false;
    for (size_t open = text.find('['); open != std::string::npos; open = text.find('[', open + 1)) {
        size_t close = text.find(']', open);
        if (close == std::string::npos) break;
        const Token* token = TokenNamed(text.substr(open + 1, close - open - 1));
        std::string label = token ? Label(token->buttons, menu) : std::string();
        if (label.empty()) continue;
        std::string before = open == 0 || SpaceAt(text, open - 1) ? "" : " ";
        std::string after = SpaceAt(text, close + 1) ? "" : " ";
        std::string named = before + label + after;
        text.replace(open, close - open + 1, named);
        open += named.size() - 1;
        changed = true;
    }
    return changed;
}

bool Letter(const std::string& text, size_t at) {
    return at < text.size() && std::isalpha(static_cast<unsigned char>(text[at]));
}

// every whole word occurrence
bool Replace(std::string& text, std::string_view from, std::string_view to) {
    bool changed = false;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
        bool word = (at == 0 || !Letter(text, at - 1)) && !Letter(text, at + from.size());
        if (!word) {
            at += from.size();
            continue;
        }
        text.replace(at, from.size(), to);
        at += to.size();
        changed = true;
    }
    return changed;
}

bool ReplaceAll(std::string& text, const Swap* swaps, size_t count) {
    bool changed = false;
    for (size_t i = 0; i < count; i++) changed |= Replace(text, swaps[i].from, swaps[i].to);
    return changed;
}

// START is the menus' Enter and play's pause key
bool NameStart(std::string& text) {
    bool changed = ReplaceAll(text, kKeyboardWording, std::size(kKeyboardWording));
    std::string label = input::CursorShown() ? "(ENTER)" : Label({kStart}, false);
    if (!label.empty()) changed |= Replace(text, kStartWord, label);
    return changed;
}

std::optional<std::string> Reworded(const char* line) {
    std::string text(line, strnlen(line, kMaxText));
    bool changed = ReplaceAll(text, kPcWording, std::size(kPcWording));
    if (KeyboardPrompts()) {
        changed |= LeadWithKey(text);
        changed |= NameKeys(text);
        changed |= NameStart(text);
    }
    if (!changed || text.size() >= kMaxText) return std::nullopt;
    return text;
}

uint32_t WriteText(const std::string& text) {
    static const uint32_t buffer = mem::AllocSystem(kMaxText);
    std::copy(text.begin(), text.end(), &G<char>(buffer));
    G<char>(buffer + uint32_t(text.size())) = 0;
    return buffer;
}

bool IsLegendText(const std::string& text) {
    return std::any_of(g_legends.texts.begin(), g_legends.texts.end(),
                       [&](const LegendText& legend) { return legend.text == text; });
}

// A relabelled text is laid out in a box sized for the console's button picture: the box grows to keep it on one
// line, about the side it is aligned to. Restored by the caller after the draw
void FitRect(uint32_t rect, float width, uint32_t align) {
    constexpr float kAlignShare[] = {0.0f, 0.5f, 1.0f};  // of the growth that goes left, by alignment
    float& box = G<float>(rect + kRectWidth);
    float extra = width - box;
    if (extra <= 0 || align >= std::size(kAlignShare)) return;
    G<float>(rect + kRectX) -= extra * kAlignShare[align];
    box = width;
}

}  // namespace

// a page of items over the case, its pause page, takes the menu cursor
bool CaseShown() { return !g_case.keys.empty() && !MenuItemsShown(); }

// before the Flash render, 0x412F1F
void HideCasePictures(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    UpdateCaseKeys(ctx);
    ctx = saved;
}

// the Flash menus are drawn, 0x412F24: the pictures back
void DrawCaseKeys(X86Context& ctx, uint8_t*) { ShowCasePictures(); }

// a clip just drawn by the Flash render: a button's keys go over it, under what draws later such as the pause page
void DrawCaseButtonKeys(X86Context& ctx, uint32_t clip) {
    auto ours = [clip](const CaseKey& key) { return key.clip == clip; };
    uint32_t manager = R32(kTextManager);
    if (std::none_of(g_case.keys.begin(), g_case.keys.end(), ours) || !manager || !R32(manager + kManagerFont)) return;
    X86Context saved = ctx;
    uint32_t previous_font = R32(manager + kManagerFont);
    uint32_t id = CallGuest(ctx, kFontId, {kDinLarge});
    ctx.ecx = manager;
    if (!(CallGuest(ctx, kSetFont, {id}) & 0xFF)) W32(manager + kManagerFont, previous_font);
    CallGuest(ctx, kNextLayer, {1});
    W32(kAppliedTexture, kNoTexture);
    for (const CaseKey& key : g_case.keys) {
        if (ours(key)) DrawCaseKey(ctx, manager, key);
    }
    W32(manager + kManagerFont, previous_font);
    ctx = saved;
}

// a view's UI widget walk, after its view index is in eax
void HudPromptsBegin(X86Context& ctx, uint8_t*) {
    uint32_t views = R32(kViewSet);
    bool split = views && R32(views + kViewCount);
    g_hud_port = split ? int(ctx.eax) : -1;
}

// the walk returns
void HudPromptsEnd(X86Context&, uint8_t*) { g_hud_port = -1; }

// the script sets a text by making a new field, so the field is known by the clip drawing it
void SetPromptField(uint32_t field) {
    bool ours = field && g_drawn_clip && EditTextField(g_drawn_clip) == field;
    g_drawn_ports.reset();
    if (ours) g_drawn_ports = PortsOf(g_drawn_clip);
}

// an Apt clip is drawn, thiscall on the clip
void LatchDrawnClip(X86Context& ctx, uint8_t*) { g_drawn_clip = ctx.ecx; }

// frame tick, after the pads are read
void MenuPrompts(X86Context& ctx, uint8_t*) {
    X86Context saved = ctx;
    UpdateMenuPorts();
    UpdateLegends(ctx);
    ctx = saved;
}

// a line of text: drawn reworded, in a box it fits
X86_EXTERN_FUNC(__imp__sub_00357060);
X86_FUNC(sub_00357060) {
    uint32_t text = R32(ctx.esp + kDrawTextArg);
    std::optional<std::string> reworded = Readable(text) ? Reworded(Text(text)) : std::nullopt;
    if (!reworded) return __imp__sub_00357060(ctx, base);
    uint32_t written = WriteText(*reworded);
    W32(ctx.esp + kDrawTextArg, written);
    uint32_t rect = R32(ctx.esp + kDrawRectArg);
    float width = LineWidth(ctx, written, G<float>(ctx.esp + kDrawScaleArg));
    float box = G<float>(rect + kRectWidth);
    std::string original(Text(text), strnlen(Text(text), kMaxText));
    if (box > 0 && IsLegendText(original)) g_label_share[original] = width / box;
    float x = G<float>(rect + kRectX);
    FitRect(rect, width, R32(ctx.esp + kDrawAlignArg));
    __imp__sub_00357060(ctx, base);
    G<float>(rect + kRectX) = x;
    G<float>(rect + kRectWidth) = box;
}

// a text measured for its box: measured as it will be drawn
X86_EXTERN_FUNC(__imp__sub_00356F90);
X86_FUNC(sub_00356F90) {
    uint32_t text = R32(ctx.esp + kDrawTextArg);
    std::optional<std::string> reworded = !g_measuring && Readable(text) ? Reworded(Text(text)) : std::nullopt;
    if (reworded) W32(ctx.esp + kDrawTextArg, WriteText(*reworded));
    __imp__sub_00356F90(ctx, base);
}
