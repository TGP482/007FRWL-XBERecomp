// Button prompts on keyboard and mouse. A menu's button legend is a Flash clip of the button's picture between two
// texts (the shadow and the text): with the keyboard or mouse used last the picture is hidden and the text leads with
// the key, "(ENTER) Select". The game draws every line of text through one call, where the legends are relabelled
// and a button the font draws as a picture ("Press[RDDown]to continue") is named by its key.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "common/mouse_keyboard.h"
#include "game/flash.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace flash;

namespace {

// text line draw, thiscall(font, rect, scale, text, align, flags)
constexpr uint32_t kDrawTextArg = 0x10;  // the text, from esp at the entry
constexpr uint32_t kMaxText = 0x400;

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

constexpr int kMaxTreeDepth = 12;
constexpr size_t kMaxVisits = 4096;
constexpr uint32_t kMaxFrameItems = 64;

// a legend's text in the language, and its button
struct LegendText {
    std::string text;
    Buttons buttons;
};

// game thread: the legends on screen this tick, and whether their pictures are hidden
struct LegendState {
    std::vector<LegendText> texts;
    bool hidden = false;
    size_t visits = 0;
};
LegendState g_legends;

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

// the picture goes while the keys are shown, the texts are kept to be relabelled as they are drawn
void ApplyLegend(uint32_t clip, const Legend& legend, bool keyboard) {
    for (uint32_t child : Children(clip)) {
        if (Type(child) == kTypeSprite) SetShown(child, !keyboard);
        uint32_t field = EditTextField(child);
        if (field && keyboard) g_legends.texts.push_back({ShownText(field), legend.buttons});
    }
}

void Visit(uint32_t clip, int depth, bool keyboard) {
    if (++g_legends.visits > kMaxVisits) return;
    if (const Legend* legend = LegendOf(clip)) return ApplyLegend(clip, *legend, keyboard);
    if (depth >= kMaxTreeDepth) return;
    for (uint32_t child : Children(clip)) Visit(child, depth + 1, keyboard);
}

void UpdateLegends() {
    bool keyboard = input::KeyboardActive();
    g_legends.texts.clear();
    if (!keyboard && !g_legends.hidden) return;
    g_legends.visits = 0;
    for (uint32_t level : Levels()) Visit(level, 0, keyboard);
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

std::optional<std::string> Reworded(const char* line) {
    std::string text(line, strnlen(line, kMaxText));
    bool changed = false;
    if (input::KeyboardActive()) changed = LeadWithKey(text) || NameKeys(text);
    if (!changed || text.size() >= kMaxText) return std::nullopt;
    return text;
}

uint32_t WriteText(const std::string& text) {
    static const uint32_t buffer = mem::AllocSystem(kMaxText);
    std::copy(text.begin(), text.end(), &G<char>(buffer));
    G<char>(buffer + uint32_t(text.size())) = 0;
    return buffer;
}

}  // namespace

// frame tick, after the pads are read
void MenuPrompts(X86Context&, uint8_t*) { UpdateLegends(); }

// a line of text: drawn reworded
X86_EXTERN_FUNC(__imp__sub_00357060);
X86_FUNC(sub_00357060) {
    uint32_t text = R32(ctx.esp + kDrawTextArg);
    std::optional<std::string> reworded = Readable(text) ? Reworded(Text(text)) : std::nullopt;
    if (reworded) W32(ctx.esp + kDrawTextArg, WriteText(*reworded));
    __imp__sub_00357060(ctx, base);
}
