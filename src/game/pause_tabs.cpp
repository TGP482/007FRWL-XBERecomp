#include "game/pause_tabs.h"

#include <algorithm>
#include <array>
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

// a child clip draw, cdecl (renderer, clip, mask)
constexpr uint32_t kDrawChild = 0x309875;
constexpr uint32_t kDrawChildAlt = 0x3098D6;  // used while bit 2 of 0x8125A8 is set
constexpr uint32_t kClipY = kClipMatrix + 5 * 4;

using Draw = void (*)(X86Context&, uint8_t*);

struct Column {
    std::array<uint32_t, kStockTabs> plates{};
    std::array<uint32_t, kStockTabs> labels{};
    float step = 0.0f;
};

struct Tabs {
    uint32_t screen = 0;
    int tab = 0;  // the focused tab, past the last stock one on a recomp tab
    bool moved = false;  // the cursor went to another recomp tab: its sound plays in the pad's turn
};

Tabs g_tabs;
const char* g_label = nullptr;
bool g_drawing = false;
bool g_column = false;  // a stock tab of the column draws

bool IsTabScreen(uint32_t screen) {
    return screen && R32(screen) == kMenuScreenVtable && R32(screen + kScreenCount) == kStockTabs;
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

// the column the clip is part of, when its page is the pause menu
bool FindColumn(uint32_t clip, Column& column) {
    uint32_t parent = R32(clip + kClipParent);
    if (!IsTabDepth(Depth(clip)) || !Readable(parent)) return false;
    std::vector<uint32_t> children = Children(parent);
    if (!HasNamed(children, "options_mc") || !HasNamed(children, "border_mc")) return false;
    for (int i = 0; i < kStockTabs; i++) {
        column.plates[i] = AtDepth(children, kPlateDepths[i]);
        column.labels[i] = AtDepth(children, kLabelDepths[i]);
        if (!column.plates[i] || !column.labels[i]) return false;
    }
    float top = G<float>(column.labels[0] + kClipY);
    column.step = (G<float>(column.labels[kLastTab] + kClipY) - top) / kLastTab;
    return true;
}

// the clip drawn moved down by some tabs
void DrawMoved(X86Context& ctx, uint32_t draw, const uint32_t args[3], uint32_t clip, float by) {
    float& y = G<float>(clip + kClipY);
    float saved = y;
    y += by;
    g_drawing = true;
    CallGuest(ctx, draw, {args[0], clip, args[2]});
    g_drawing = false;
    y = saved;
}

// a blue plate to copy: one the stock focus is not on
int BlueTab(int stock) { return stock == 0 ? 1 : 0; }

// the recomp's tabs, after the column's last stock clip so their plates and labels keep the column's order
void DrawExtraTabs(X86Context& ctx, uint32_t draw, const uint32_t args[3], const Column& column, int stock) {
    auto from = [&](int tab) { return tab == g_tabs.tab ? stock : BlueTab(stock); };
    for (int tab = kStockTabs; tab < kTabCount; tab++) {
        DrawMoved(ctx, draw, args, column.plates[from(tab)], column.step * float(tab - from(tab)));
    }
    for (int tab = kStockTabs; tab < kTabCount; tab++) {
        g_label = kLabels[tab - kStockTabs];
        DrawMoved(ctx, draw, args, column.labels[from(tab)], column.step * float(tab - from(tab)));
    }
    g_label = nullptr;
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
    if (away && args[1] == column.plates[stock]) {
        // the red plate moves to the recomp tab, a blue one takes its place
        int blue = BlueTab(stock);
        X86Context saved = ctx;
        DrawMoved(ctx, draw, args, column.plates[blue], column.step * float(stock - blue));
        ctx = saved;
        Return(ctx, 0, 0);
        return;
    }
    g_column = true;
    original(ctx, base);
    g_column = false;
    if (args[1] != column.labels[0]) return;
    X86Context saved = ctx;
    DrawExtraTabs(ctx, draw, args, column, stock);
    ctx = saved;
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
    if (!IsTabScreen(screen)) return;
    int stock = int(R32(screen + kScreenIndex));
    if (screen != g_tabs.screen) g_tabs = {screen, stock};
    if (stock != kLastTab || g_tabs.tab < kLastTab) g_tabs.tab = stock;
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

const char* PauseTabLabel() { return g_label; }

bool DrawingPauseTab() { return g_column; }

// both of the player's child draws: the tab column's clips draw the recomp's tabs with them, the attache case's
// buttons their keys
X86_EXTERN_FUNC(__imp__sub_00309875);
X86_FUNC(sub_00309875) { DrawChild(ctx, base, __imp__sub_00309875, kDrawChild); }

X86_EXTERN_FUNC(__imp__sub_003098D6);
X86_FUNC(sub_003098D6) { DrawChild(ctx, base, __imp__sub_003098D6, kDrawChildAlt); }
