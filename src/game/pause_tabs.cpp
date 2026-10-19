#include "game/pause_tabs.h"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "game/flash.h"
#include "game/options_menu.h"

using namespace kernel;
using namespace flash;

void DrawCaseButtonKeys(X86Context& ctx, uint32_t clip);  // prompts.cpp

namespace {

// tab screen
constexpr uint32_t kMenuScreenVtable = 0x6B1A90;
constexpr uint32_t kScreenRows = 0xB4;
constexpr uint32_t kScreenCount = 0xBC;
constexpr uint32_t kScreenColumn = 0xC0;
constexpr uint32_t kScreenRow = 0xC4;
constexpr uint32_t kScreenIndex = 0xC8;
constexpr uint32_t kScreenAccept = 0xCC;
constexpr uint32_t kScreenSelect = 0x1C847;  // thiscall: OnSelect of the row set
constexpr uint32_t kButtonLeft = 2;
constexpr uint32_t kButtonRight = 3;
constexpr uint32_t kButtonUp = 4;
constexpr uint32_t kButtonDown = 5;

// the page's tab plates and their labels, top to bottom; the focused plate is red
constexpr int kPlateDepths[kStockTabs] = {124, 85, 46, 7};
constexpr int kLabelDepths[kStockTabs] = {162, 121, 82, 43};
constexpr int kLastTab = kStockTabs - 1;
constexpr int kTabCount = kStockTabs + kExtraTabs;
const char* const kLabels[kExtraTabs] = {"RECOMP", "M & K", "CONTROLLER"};  // pages in rows::Page order
const char* const kStar = "star_mc";  // beside the focused tab
const char* const kBorder = "border_mc";  // drawn after every tab, the recomp's tabs just before it

// a child clip draw, cdecl (renderer, clip, mask)
constexpr uint32_t kDrawChild = 0x309875;
constexpr uint32_t kDrawChildAlt = 0x3098D6;  // used while bit 2 of 0x8125A8 is set
constexpr uint32_t kClipX = kClipMatrix + 4 * 4;
constexpr uint32_t kClipY = kClipMatrix + 5 * 4;

using Draw = void (*)(X86Context&, uint8_t*);

using Clips = std::array<uint32_t, kStockTabs>;
using Places = std::array<float, kStockTabs>;

struct Column {
    Clips plates{};
    Clips labels{};
    int blue = 0;  // the blue tab copies are made from
};

struct Tabs {
    uint32_t screen = 0;
    int tab = 0;  // the focused tab, past the last stock one on a recomp tab
    bool moved = false;  // the cursor went to another recomp tab: its sound plays in the pad's turn
    int stock = -1;        // the stock focus last frame
    uint32_t frame = 0;
    std::array<uint32_t, kStockTabs> changed{};  // the frame each tab last gained or lost the focus
    // a blue tab's x at rest, from one not focused since the menu opened: a focus slides a tab right and back
    std::optional<float> plate_x;
    std::optional<float> label_x;
};

Tabs g_tabs;
// the stock tabs' heights as last seen: they keep them, but a focus change swaps some clips out for a moment
Places g_plate_y{};
Places g_label_y{};
std::string g_label;  // the text a tab label copy shows, in the game's text
bool g_relabel = false;
bool g_drawing = false;
bool g_column = false;  // a stock tab of the column draws

// a closed menu's screen may already be freed
bool IsTabScreen(uint32_t screen) {
    return Readable(screen) && R32(screen) == kMenuScreenVtable && R32(screen + kScreenCount) == kStockTabs;
}

int StockIndex() { return IsTabScreen(g_tabs.screen) ? int(R32(g_tabs.screen + kScreenIndex)) : -1; }

bool IsTabDepth(int depth) {
    for (int i = 0; i < kStockTabs; i++) {
        if (depth == kPlateDepths[i] || depth == kLabelDepths[i]) return true;
    }
    return false;
}

uint32_t AtDepth(const std::vector<uint32_t>& clips, int depth) {
    for (uint32_t clip : clips) {
        if (Depth(clip) == depth) return clip;
    }
    return 0;
}

// unnamed clips have no name string
bool HasNamed(const std::vector<uint32_t>& clips, const char* name) {
    return std::any_of(clips.begin(), clips.end(), [name](uint32_t clip) {
        uint32_t text = R32(clip + kClipName);
        return Readable(text) && _stricmp(Text(text + kStringText), name) == 0;
    });
}

// the tab whose focus changed longest ago: a tab's look animates for a while after its focus changes
int RestingTab(const Column& column, int stock) {
    int best = -1;
    for (int tab = 0; tab < kStockTabs; tab++) {
        if (tab == stock || !column.plates[size_t(tab)] || !column.labels[size_t(tab)]) continue;
        if (best < 0 || g_tabs.changed[size_t(tab)] < g_tabs.changed[size_t(best)]) best = tab;
    }
    return best;
}

float ClipY(uint32_t clip) { return G<float>(clip + kClipY); }

// a tab's place: the stock ones where they are, focused or not, the recomp ones a step apart past the last
float TabY(const Places& places, int tab) {
    if (tab <= kLastTab) return places[size_t(tab)];
    float first = places[0];
    float last = places[kLastTab];
    return last + (last - first) / kLastTab * float(tab - kLastTab);
}

// how far a copy of tab from's clip moves to tab's place
float MoveBy(const Places& places, int from, int tab) { return TabY(places, tab) - places[size_t(from)]; }

float ClipX(uint32_t clip) { return G<float>(clip + kClipX); }

// the first tab seen that was never focused gives the rest x
void LatchRestX(const Column& column, int stock) {
    for (int tab = 0; tab < kStockTabs && !g_tabs.plate_x; tab++) {
        bool never = tab != stock && g_tabs.changed[size_t(tab)] == 0;
        if (!never || !column.plates[size_t(tab)] || !column.labels[size_t(tab)]) continue;
        g_tabs.plate_x = ClipX(column.plates[size_t(tab)]);
        g_tabs.label_x = ClipX(column.labels[size_t(tab)]);
    }
}

// the tab column of the page, when the page is the pause menu
bool ColumnOf(uint32_t page, Column& column) {
    if (!Readable(page)) return false;
    std::vector<uint32_t> children = Children(page);
    if (!HasNamed(children, "options_mc") || !HasNamed(children, kBorder)) return false;
    for (int i = 0; i < kStockTabs; i++) {
        column.plates[i] = AtDepth(children, kPlateDepths[i]);
        column.labels[i] = AtDepth(children, kLabelDepths[i]);
        if (column.plates[i]) g_plate_y[size_t(i)] = ClipY(column.plates[i]);
        if (column.labels[i]) g_label_y[size_t(i)] = ClipY(column.labels[i]);
    }
    column.blue = RestingTab(column, StockIndex());
    LatchRestX(column, StockIndex());
    return column.blue >= 0;
}

// the column the clip is part of: a tab's plate or label, or the star beside them
bool FindColumn(uint32_t clip, Column& column) {
    bool tab = IsTabDepth(Depth(clip)) || HasNamed({clip}, kStar) || HasNamed({clip}, kBorder);
    return tab && ColumnOf(R32(clip + kClipParent), column);
}

// the clip drawn moved down by some tabs
// the clip drawn moved down, and at the x given
void DrawMoved(X86Context& ctx, uint32_t draw, const uint32_t args[3], uint32_t clip, float by,
               std::optional<float> at = std::nullopt) {
    float& x = G<float>(clip + kClipX);
    float& y = G<float>(clip + kClipY);
    float saved_x = x;
    float saved_y = y;
    y += by;
    if (at) x = *at;
    g_drawing = true;
    CallGuest(ctx, draw, {args[0], clip, args[2]});
    g_drawing = false;
    x = saved_x;
    y = saved_y;
}


// the text a tab label shows, from its own text field
std::string ShownText(uint32_t label) {
    for (uint32_t child : Children(label)) {
        uint32_t field = EditTextField(child);
        if (!field) continue;
        uint32_t text = R32(field + kFieldShownText);
        if (!Readable(text) || !*Text(text)) text = R32(field + kFieldText);
        return Readable(text) ? Text(text) : "";
    }
    return "";
}

// a blue label copy in a tab's place, with the text given
void DrawLabel(X86Context& ctx, uint32_t draw, const uint32_t args[3], uint32_t from, float by, std::string text,
               std::optional<float> at) {
    g_label = std::move(text);
    g_relabel = true;
    DrawMoved(ctx, draw, args, from, by, at);
    g_relabel = false;
}

// the recomp's tabs, after the column's last stock clip so their plates and labels keep the column's order
void DrawExtraTabs(X86Context& ctx, uint32_t draw, const uint32_t args[3], const Column& column, int stock) {
    // the red copy keeps its source's slide, blue ones sit at rest
    auto red = [&](int tab) { return tab == g_tabs.tab; };
    auto from = [&](int tab) { return red(tab) ? stock : column.blue; };
    for (int tab = kStockTabs; tab < kTabCount; tab++) {
        uint32_t plate = column.plates[size_t(from(tab))];
        std::optional<float> at = red(tab) ? std::nullopt : g_tabs.plate_x;
        if (plate) DrawMoved(ctx, draw, args, plate, MoveBy(g_plate_y, from(tab), tab), at);
    }
    for (int tab = kStockTabs; tab < kTabCount; tab++) {
        uint32_t label = column.labels[size_t(from(tab))];
        if (!label) continue;
        float by = MoveBy(g_label_y, from(tab), tab);
        std::optional<float> at = red(tab) ? std::nullopt : g_tabs.label_x;
        DrawLabel(ctx, draw, args, label, by, MenuText(kLabels[tab - kStockTabs]), at);
    }
}

void DrawChild(X86Context& ctx, uint8_t* base, Draw original, uint32_t draw) {
    uint32_t args[3] = {R32(ctx.esp + 4), R32(ctx.esp + 8), R32(ctx.esp + 12)};
    if (HideOptionsClip(args[1])) {
        Return(ctx, 0, 0);
        return;
    }
    Column column;
    int stock = StockIndex();
    if (g_drawing || stock < 0 || !FindColumn(args[1], column)) {
        original(ctx, base);
        return DrawCaseButtonKeys(ctx, args[1]);
    }
    bool away = g_tabs.tab != stock;
    if (away && HasNamed({args[1]}, kStar)) {
        X86Context saved = ctx;
        float by = TabY(g_label_y, g_tabs.tab) - TabY(g_label_y, stock);
        DrawMoved(ctx, draw, args, args[1], by);
        ctx = saved;
        Return(ctx, 0, 0);
        return;
    }
    // the red plate and its label move to the recomp tab, blue ones take their place
    int blue = column.blue;
    if (away && args[1] == column.plates[stock]) {
        X86Context saved = ctx;
        DrawMoved(ctx, draw, args, column.plates[blue], MoveBy(g_plate_y, blue, stock), g_tabs.plate_x);
        ctx = saved;
        Return(ctx, 0, 0);
        return;
    }
    if (away && args[1] == column.labels[stock]) {
        X86Context saved = ctx;
        float by = MoveBy(g_label_y, blue, stock);
        DrawLabel(ctx, draw, args, column.labels[blue], by, ShownText(args[1]), g_tabs.label_x);
        ctx = saved;
        Return(ctx, 0, 0);
        return;
    }
    if (HasNamed({args[1]}, kBorder)) {
        X86Context saved = ctx;
        DrawExtraTabs(ctx, draw, args, column, stock);
        ctx = saved;
        original(ctx, base);
        return;
    }
    g_column = true;
    original(ctx, base);
    g_column = false;
}

void Select(X86Context& ctx, int index) {
    W32(g_tabs.screen + kScreenRow, uint32_t(index));
    W32(g_tabs.screen + kScreenColumn, 0);
    ctx.ecx = g_tabs.screen;
    CallGuest(ctx, kScreenSelect, {});
}

// up and down step over every tab; the stock screen steps between its own
bool Step(X86Context& ctx, int step) {
    int tab = g_tabs.tab + step;
    if (tab >= kLastTab && tab < kTabCount && g_tabs.tab >= kLastTab) {
        g_tabs.tab = tab;
        ReselectRow(ctx, g_tabs.screen);
        return true;
    }
    if (tab == kTabCount || tab < 0) {
        g_tabs.tab = tab < 0 ? kTabCount - 1 : 0;
        Select(ctx, std::min(g_tabs.tab, kLastTab));
        return true;
    }
    return false;
}

}  // namespace

void UpdatePauseTabs(uint32_t screen) {
    if (!IsTabScreen(screen)) {
        // menu closed: every clip draw reads the kept screen, which the game frees
        g_tabs.screen = 0;
        return;
    }
    int stock = int(R32(screen + kScreenIndex));
    if (screen != g_tabs.screen) g_tabs = {screen, stock};
    if (stock != kLastTab || g_tabs.tab < kLastTab) g_tabs.tab = stock;
    g_tabs.frame++;
    if (stock == g_tabs.stock) return;
    for (int tab : {g_tabs.stock, stock}) {
        if (tab >= 0 && tab < kStockTabs) g_tabs.changed[size_t(tab)] = g_tabs.frame;
    }
    g_tabs.stock = stock;
}

void PointPauseTab(int tab) {
    if (tab < 0 || tab >= kTabCount) return;
    // between recomp tabs the stock screen does not move, so it plays no sound itself
    g_tabs.moved = g_tabs.moved || (tab != g_tabs.tab && tab >= kLastTab && g_tabs.tab >= kLastTab);
    g_tabs.tab = tab;
}

void PauseTabsFocus(X86Context& ctx) {
    if (!g_tabs.moved) return;
    g_tabs.moved = false;
    if (IsTabScreen(g_tabs.screen)) ReselectRow(ctx, g_tabs.screen);
}

bool TakePauseTabButton(X86Context& ctx, uint32_t screen, uint32_t code) {
    if (!IsTabScreen(screen) || screen != g_tabs.screen) return false;
    if (code == R32(screen + kScreenAccept)) {
        // the stock OPTIONS opens, its panel then opens the page
        OpenOptionsPage(g_tabs.tab - kStockTabs);
        return false;
    }
    bool rows = R32(screen + kScreenRows) == kStockTabs;
    uint32_t back = rows ? kButtonUp : kButtonLeft;
    uint32_t next = rows ? kButtonDown : kButtonRight;
    if (code != back && code != next) return false;
    return Step(ctx, code == next ? 1 : -1);
}

bool HoldPauseTabButton(uint32_t screen) {
    return IsTabScreen(screen) && screen == g_tabs.screen && g_tabs.tab >= kLastTab;
}

const char* PauseTabLabel() { return g_relabel ? g_label.c_str() : nullptr; }

bool DrawingPauseTab() { return g_column; }

int PauseTabPage() {
    bool recomp = StockIndex() == kLastTab && g_tabs.tab > kLastTab;
    return recomp ? g_tabs.tab - kStockTabs : -1;
}

const char* PauseTabTitle() {
    static std::string title;
    int page = PauseTabPage();
    if (page < 0) return nullptr;
    title = MenuText(kLabels[page]);
    return title.c_str();
}

// both of the player's child draws: the tab column's clips draw the recomp's tabs with them, the attache case's
// buttons their keys
X86_EXTERN_FUNC(__imp__sub_00309875);
X86_FUNC(sub_00309875) { DrawChild(ctx, base, __imp__sub_00309875, kDrawChild); }

X86_EXTERN_FUNC(__imp__sub_003098D6);
X86_FUNC(sub_003098D6) { DrawChild(ctx, base, __imp__sub_003098D6, kDrawChildAlt); }
