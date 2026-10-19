#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/translations.h"
#include "game/flash.h"
#include "game/menu_ports.h"
#include "game/options_menu.h"
#include "game/pause_tabs.h"
#include "xdk/hle.h"

// Recomp settings in the game's own options menu, the front end's and the pause menu's. Entries under the front end's
// stock rows open pages with the F10 panel's rows, the pause menu's tabs open them straight away. A page with
// categories lists them first, each opens its rows. A page row is one line, its name left and its value right, drawn
// as copies of a stock row's text; the stock menu screen keeps the cursor, so the rows look and sound as the game's own.

using namespace kernel;
using app::rows::Kind;
using app::rows::Row;

namespace {

// menu screen made by the script's CreateMenu (constructor 0x1C6B40)
constexpr uint32_t kMenuScreenVtable = 0x6B1A90;
constexpr uint32_t kScreenTakesInput = 0x20;  // 1 while it takes input
constexpr uint32_t kScreenRows = 0xB4;
constexpr uint32_t kScreenColumns = 0xB8;
constexpr uint32_t kScreenCount = 0xBC;  // rows times columns
constexpr uint32_t kScreenColumn = 0xC0;
constexpr uint32_t kScreenRow = 0xC4;
constexpr uint32_t kScreenIndex = 0xC8;  // row times columns plus column
constexpr uint32_t kScreenAccept = 0xCC;  // the button code that activates a row
constexpr uint32_t kScreenCancel = 0xD0;
constexpr uint32_t kScreenBlocked = 0xDC;    // BlockInput
constexpr uint32_t kScreenSelect = 0x1C847;  // thiscall: wraps the row and column into the index, then OnSelect
constexpr uint32_t kScreenScript = 0x88;      // the page script's handler
constexpr uint32_t kScriptOnSelect = 0x08;    // vtable slots, thiscall (index)
constexpr uint32_t kScriptOnDeselect = 0x0C;

// button codes the screens get (switch at 0x1C57D8): the column goes left and right, the row up and down
constexpr uint32_t kButtonLeft = 2;
constexpr uint32_t kButtonRight = 3;
constexpr uint32_t kButtonUp = 4;
constexpr uint32_t kButtonDown = 5;

// every live UI object, the menu screens among them
constexpr uint32_t kObjects = 0x8375E4;
constexpr uint32_t kObjectCount = 0x8375E8;

// Apt text field render data, made at 0x413D80 and drawn by kDrawField
constexpr uint32_t kFieldLeft = 0x04;  // the box: left, top, right, bottom, floats in the field's own space
constexpr uint32_t kFieldTop = 0x08;
constexpr uint32_t kFieldRight = 0x0C;
constexpr uint32_t kFieldBottom = 0x10;
constexpr uint32_t kFieldAlign = 0x14;   // 0 left, 1 right, 2 centred
constexpr uint32_t kFieldColour = 0x2C;  // ARGB
constexpr uint32_t kFieldRaw = 0x54;     // the text as the movie sets it, "$ID" for a string table entry
constexpr uint32_t kFieldShown = 0x74;   // the text drawn when set
constexpr uint32_t kFieldBytes = 0x7C;
constexpr uint32_t kDrawField = 0x413E50;  // cdecl (field, unused)
constexpr uint32_t kAlignLeft = 0;
constexpr uint32_t kAlignRight = 1;

// Apt renderer while a character draws: +4 its matrix, translation in screen pixels (y down) at +0x30 and +0x34
constexpr uint32_t kRenderer = 0x8375FC;
constexpr uint32_t kRendererMatrix = 4;  // 4x4, row major
constexpr uint32_t kMatrixScaleX = 0x00;
constexpr uint32_t kMatrixScaleY = 0x14;
constexpr uint32_t kMatrixX = 0x30;
constexpr uint32_t kMatrixY = 0x34;
// colour transform of the drawn character: four multipliers, four offsets
constexpr uint32_t kCxformMul = 0x837D94;
constexpr uint32_t kCxformAdd = 0x837D30;
constexpr int kCxformWords = 4;

// a held direction, as the screen steps it (0x1C58A0): the code held, -1 for none, and the wait to its next step
constexpr uint32_t kScreenHeldCode = 0xE4;
constexpr uint32_t kScreenHeldWait = 0xE8;
constexpr uint32_t kHeldDelay = 0x6921DC;      // float: the wait after each step
constexpr uint32_t kFrameSeconds = 0x824358;  // float
constexpr uint32_t kNoCode = 0xFFFFFFFF;

// the pause menu's title shows the recomp tab's name
const char* const kPauseTitle = "$PAUSE_OPTIONS";

// the stock rows' texts, in any order: the rows are ordered by their place on screen
const char* const kFrontEndRows[] = {"$OPTIONS_BRIGHTNESS", "$OPTIONS_CREDITS", "$OPTIONS_NFS"};
constexpr int kFrontEnd = 0;  // in kMenus
constexpr int kNfsDemoLabel = 2;  // its native LaunchDemo (0x1B78A0) does nothing, the page would wait on it for good
const char* const kPauseRows[] = {
    "$OPTIONS_VIBRATION",
    "$OPTIONS_INVERTHOR",
    "$OPTIONS_INVERTVER",
    "$OPTIONS_SUBTITLES",
    "$OPTIONS_RESTOREDEFAULTS",
    "$OPTIONS_QUITLEVEL",
    "$UI_RESTART",
};
struct Menu {
    const char* const* labels;
    int count;
};
const Menu kMenus[] = {
    {kFrontEndRows, int(std::size(kFrontEndRows))},
    {kPauseRows, int(std::size(kPauseRows))},
};
constexpr int kPause = 1;
constexpr int kMaxRows = 8;
constexpr size_t kMaxRaw = 64;

// an entry's page is rows::Page of its index
const char* const kEntries[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD", "CONTROLLER"};
constexpr int kEntryCount = int(std::size(kEntries));
const char* const kDefaultsLabel = "RESTORE DEFAULTS";

constexpr int kForgetFrames = 15;  // a menu not drawn this many frames is closed
constexpr uint32_t kTextBytes = 128;
constexpr int kMaxFields = 4;     // text fields of one part of a row, shadows included
constexpr float kWiden = 3.0f;    // a copy's box grows by this many of its widths, so a long text keeps one line

// a page row: its name and value span the options panel's column, in screen pixels, at a smaller text
constexpr float kColumnHalf = 160.0f;
constexpr float kPageTextScale = 0.7f;
constexpr int kStageWidth = 640;  // the screen the Flash stage is drawn across
constexpr int kStageHeight = 480;
constexpr uint32_t kStageWidthNow = 0x7626E8;  // float: the stage's own size
constexpr uint32_t kStageHeightNow = 0x7626EC;

// on a page the pause menu's rows drop what they hold besides their labels: the value boxes and arrows
const char* const kOptionsPanel = "options_mc";
constexpr int kMinLines = 9;
constexpr int kMaxPanelLevels = 8;
constexpr int kLabelLevels = 3;  // how deep a row's clip may keep its label

enum Part { kLabel, kValue, kPartCount };

struct Place {
    float x;
    float y;
};

// the stock labels one frame drew, by their index in the menu's list
struct Sight {
    int menu = -1;
    uint32_t labels = 0;
    std::array<Place, kMaxRows> place{};
    std::array<std::array<std::string, kPartCount>, kMaxRows> text{};  // by the last frame's rows
};

// the stock rows of the last frame in screen order
struct Layout {
    int menu = -1;
    int stock = 0;
    std::array<Place, kMaxRows> row{};
    std::array<int, kMaxRows> row_of{};  // the row of each label
    Place spacing{};
    std::array<std::array<std::string, kPartCount>, kMaxRows> text{};  // each row's label and value as shown
};

// a stock row's text field as it is drawn
struct Field {
    int row;
    Part part;
    int order;  // among the row's fields of that part this frame
};

using Cxform = std::array<uint32_t, 2 * kCxformWords>;

// how a field of a row looks with the cursor on the row or off it
struct Look {
    uint32_t colour = 0;
    Cxform cxform{};
    bool seen = false;
};

// the menu screen the rows belong to
struct State {
    uint32_t screen = 0;
    int page = -1;  // the open page, -1 while the list shows
    int top = 0;    // the page row in the first slot
    int list_top = 0;  // the list row in the first slot, past 0 once the cursor goes below the stock rows
    int missing = 0;  // frames the menu was not drawn
    bool direct = false;  // the page opened from a pause tab, its back leaves the menu
    const Row* section = nullptr;  // the category open, null for the page's list of them
    std::vector<const Row*> rows;  // the page's rows on show; null restores the page's defaults
};

// where a page's lines are hit, from the last one drawn: screen pixels
struct Lines {
    float middle_x = 0.0f;
    float middle_dy = 0.0f;  // the text's middle below its line's place
    bool seen = false;
};

// all on the game thread
Sight g_now;
Layout g_layout;
State g_state;
std::array<std::array<int, kPartCount>, kMaxRows> g_order{};
Look g_looks[kPartCount][2][kMaxFields];
bool g_copying = false;
int g_tab_page = -1;  // the page a pause tab asked for, opened once its menu shows
Lines g_lines;
std::array<uint32_t, kMaxRows> g_changed{};  // the frame each stock row last gained or lost the cursor
uint32_t g_frame = 0;
int g_last_index = -1;
uint32_t g_panel = 0;  // the options screen, taking input or not

bool PageOpen() { return g_state.page >= 0; }

int CursorIndex() { return g_state.screen ? int(R32(g_state.screen + kScreenIndex)) : -1; }

// rows show in the stock rows' places, scrolling past them
int Slots() { return g_layout.stock; }

// the front end's demo row is in another font
bool Copyable(int row) { return g_layout.menu != kFrontEnd || row != g_layout.row_of[kNfsDemoLabel]; }

// the stock rows' focus: the options screen keeps it while a recomp tab only shows its page
int StockFocus() {
    uint32_t screen = g_state.screen ? g_state.screen : g_panel;
    return screen ? int(R32(screen + kScreenIndex)) : -1;
}

// the stock row copies are made from: not the focused one, and the one whose focus changed longest ago, as rows
// animate when it does
int SourceRow() {
    int best = -1;
    for (int row = 0; row < g_layout.stock; row++) {
        if (row == StockFocus() || !Copyable(row)) continue;
        if (best < 0 || g_changed[size_t(row)] < g_changed[size_t(best)]) best = row;
    }
    return std::max(best, 0);
}

Place DrawnPlace() {
    uint32_t matrix = R32(R32(kRenderer) + kRendererMatrix);
    return {G<float>(matrix + kMatrixX), G<float>(matrix + kMatrixY)};
}

Place SlotPlace(int slot) { return g_layout.row[size_t(std::clamp(slot, 0, g_layout.stock - 1))]; }

// the pause menu's pages are its tabs
int EntryCount() { return g_layout.menu == kPause ? 0 : kEntryCount; }

// the front end's list is drawn as lines too, its entries after the stock rows
bool ListShown() { return !PageOpen() && EntryCount() > 0; }

// lines are as far apart as the stock rows nearest each other, and at least kMinLines fit the rows' height
float LineStep() {
    float step = (SlotPlace(Slots() - 1).y - SlotPlace(0).y) / float(kMinLines - 1);
    for (int row = 1; row < Slots(); row++) step = std::min(step, SlotPlace(row).y - SlotPlace(row - 1).y);
    return step;
}

// as many lines as fit in the stock rows' height
int LineSlots() {
    float step = LineStep();
    float height = SlotPlace(Slots() - 1).y - SlotPlace(0).y;
    return step > 0.0f ? int(height / step) + 1 : Slots();
}

float LineY(int slot) { return SlotPlace(0).y + LineStep() * float(slot); }

int ListSlots() { return std::min(g_layout.stock + EntryCount(), LineSlots()); }

// Pages.

// the page's categories, its headings, when it has more than one
std::vector<const Row*> Categories(int page) {
    size_t count = 0;
    const Row* rows = app::rows::Page(page, &count);
    std::vector<const Row*> list;
    for (size_t i = 0; i < count; i++) {
        if (rows[i].kind == Kind::Heading) list.push_back(&rows[i]);
    }
    if (list.size() < 2) list.clear();
    return list;
}

// the categories, or the open category's rows; the defaults row ends the page's first list
std::vector<const Row*> PageList(int page, const Row* section) {
    std::vector<const Row*> list = Categories(page);
    if (!list.empty() && !section) {
        list.push_back(nullptr);
        return list;
    }
    list.clear();
    size_t count = 0;
    const Row* rows = app::rows::Page(page, &count);
    bool inside = !section;
    for (size_t i = 0; i < count; i++) {
        bool heading = rows[i].kind == Kind::Heading;
        if (heading && section) inside = &rows[i] == section;
        if (!heading && inside) list.push_back(&rows[i]);
    }
    if (!section) list.push_back(nullptr);
    return list;
}

void FillRows() { g_state.rows = PageList(g_state.page, g_state.section); }

// a recomp pause tab the cursor is on shows its page's first rows, as the stock tabs show their panels
int PreviewPage() { return PageOpen() || g_layout.menu != kPause ? -1 : PauseTabPage(); }

int PageRowCount() { return int(g_state.rows.size()); }

// the slots the page shows at once, the screen's rows
int PageSlots() { return std::min(PageRowCount(), LineSlots()); }

bool HasRow(int index) { return index >= 0 && index < PageRowCount(); }

const Row* PageRow(int index) { return HasRow(index) ? g_state.rows[size_t(index)] : nullptr; }

int FocusedRow() { return g_state.top + CursorIndex(); }

}  // namespace

// the script deselects and selects the focused row again, as a move does, with its sound
void ReselectRow(X86Context& ctx, uint32_t screen) {
    uint32_t script = R32(screen + kScreenScript);
    if (!script || R32(screen + kScreenTakesInput) != 1) return;
    uint32_t index = R32(screen + kScreenIndex);
    for (uint32_t slot : {kScriptOnDeselect, kScriptOnSelect}) {
        ctx.ecx = script;
        CallGuest(ctx, R32(R32(script) + slot), {index});
    }
}

// the translations are all Latin 1, two UTF-8 bytes at most
std::string MenuText(std::string_view english, bool caps) {
    std::string utf8 = app::Ui(english, caps);
    std::string text;
    for (size_t i = 0; i < utf8.size(); i++) {
        uint8_t c = uint8_t(utf8[i]);
        bool pair = (c & 0xE0) == 0xC0 && i + 1 < utf8.size();
        if (!pair) {
            text += char(c);
            continue;
        }
        text += char((c & 0x1F) << 6 | (uint8_t(utf8[i + 1]) & 0x3F));
        i++;
    }
    return text;
}

namespace {

std::string RowLabel(const Row* row) { return MenuText(row ? row->label : kDefaultsLabel); }

std::string RowValue(const Row* row) {
    if (!row || row->kind == Kind::Heading) return std::string();
    std::string value = app::rows::Value(*row);
    // key names as they are
    bool bound = row->kind == Kind::Bind || row->kind == Kind::PadBind;
    bool keys = bound && value != "UNBOUND" && value != "PRESS A KEY" && value != "PRESS A BUTTON";
    return keys ? value : MenuText(value);
}

// The screen.


int WantedRows() { return PageOpen() ? PageSlots() : g_layout.stock + EntryCount(); }

void SetRows(int rows) {
    W32(g_state.screen + kScreenRows, uint32_t(rows));
    W32(g_state.screen + kScreenCount, uint32_t(rows));
}

// as the screen's own input moves it: OnDeselect and OnSelect follow, and their sound when the cursor stays
void MoveCursor(X86Context& ctx, int index) {
    if (index == CursorIndex()) return ReselectRow(ctx, g_state.screen);
    W32(g_state.screen + kScreenRow, uint32_t(index));
    W32(g_state.screen + kScreenColumn, 0);
    ctx.ecx = g_state.screen;
    CallGuest(ctx, kScreenSelect, {});
}

// the page's rows made again, the cursor on the row given
void ShowRows(X86Context& ctx, int focus) {
    FillRows();
    int slots = PageSlots();
    focus = std::clamp(focus, 0, PageRowCount() - 1);
    g_state.top = std::min(focus, PageRowCount() - slots);
    SetRows(slots);
    MoveCursor(ctx, focus - g_state.top);
}

void OpenPage(X86Context& ctx, int page) {
    g_state.page = page;
    g_state.section = nullptr;
    ShowRows(ctx, 0);
}

void OpenSection(X86Context& ctx, const Row* heading) {
    g_state.section = heading;
    ShowRows(ctx, 0);
}

// back on the categories, the cursor on the one left
void CloseSection(X86Context& ctx) {
    const Row* left = g_state.section;
    g_state.section = nullptr;
    FillRows();
    auto at = std::find(g_state.rows.begin(), g_state.rows.end(), left);
    ShowRows(ctx, int(at - g_state.rows.begin()));
}

void ClosePage(X86Context& ctx) {
    int entry = g_state.page;
    g_state.page = -1;
    g_state.rows.clear();
    SetRows(g_layout.stock + EntryCount());
    MoveCursor(ctx, g_layout.stock + entry);
}

// a page a pause tab opened goes as the screen's own back leaves it
void LeavePage() {
    g_state.page = -1;
    g_state.section = nullptr;
    g_state.direct = false;
    g_state.rows.clear();
    SetRows(g_layout.stock + EntryCount());
}

// up and down move the cursor over the slots and scroll the page at their ends
void MovePageCursor(X86Context& ctx, int step) {
    int slots = PageSlots();
    int last_top = PageRowCount() - slots;
    int slot = CursorIndex() + step;
    if (slot < 0 && g_state.top > 0) {
        g_state.top--;
        MoveCursor(ctx, 0);
    } else if (slot >= slots && g_state.top < last_top) {
        g_state.top++;
        MoveCursor(ctx, slots - 1);
    } else if (slot < 0) {
        g_state.top = last_top;
        MoveCursor(ctx, slots - 1);
    } else if (slot >= slots) {
        g_state.top = 0;
        MoveCursor(ctx, 0);
    } else {
        MoveCursor(ctx, slot);
    }
}

void StepRow(X86Context& ctx, int step) {
    const Row* row = PageRow(FocusedRow());
    if (!row || row->kind != Kind::Value) return;
    app::rows::Value(*row, step);
    ReselectRow(ctx, g_state.screen);
}

// Delete on a binding row
void UnbindFocusedRow() {
    const Row* row = PageRow(FocusedRow());
    if (row && row->kind == Kind::Bind) input::Unbind(size_t(row->id));
}

void ActivateRow(X86Context& ctx) {
    int index = FocusedRow();
    const Row* row = PageRow(index);
    if (!HasRow(index)) return;
    if (row && row->kind == Kind::Heading) return OpenSection(ctx, row);
    if (!row) {
        app::rows::Defaults(g_state.page);
    } else if (row->kind == Kind::Value) {
        app::rows::Value(*row, 1);
    } else {
        app::rows::Bind(*row);
    }
    ReselectRow(ctx, g_state.screen);
}

// back: a category goes back to the page's list, a page to the menu's own list
bool TakeBack(X86Context& ctx) {
    if (g_state.section) {
        CloseSection(ctx);
        return true;
    }
    if (g_state.direct) {
        LeavePage();
        return false;
    }
    ClosePage(ctx);
    return true;
}

// a direction on a page, pressed or repeating
bool TakeDirection(X86Context& ctx, uint32_t code) {
    if (code == kButtonUp || code == kButtonDown) {
        MovePageCursor(ctx, code == kButtonUp ? -1 : 1);
    } else if (code == kButtonLeft || code == kButtonRight) {
        StepRow(ctx, code == kButtonLeft ? -1 : 1);
    } else {
        return false;
    }
    return true;
}

// a button on a page: the page keeps every button the screen would act on
bool TakePageButton(X86Context& ctx, uint32_t code) {
    if (code == R32(g_state.screen + kScreenCancel)) return TakeBack(ctx);
    if (code == R32(g_state.screen + kScreenAccept)) {
        ActivateRow(ctx);
        return true;
    }
    return TakeDirection(ctx, code);
}

// a button on the list: the entries open their page
bool OnNfsDemo() { return g_layout.menu == kFrontEnd && CursorIndex() == g_layout.row_of[kNfsDemoLabel]; }

bool TakeListButton(X86Context& ctx, uint32_t code) {
    bool accept = code == R32(g_state.screen + kScreenAccept);
    if (accept && OnNfsDemo()) return true;
    int entry = CursorIndex() - g_layout.stock;
    bool on_entry = entry >= 0 && entry < EntryCount();
    if (!on_entry || !accept) return false;
    OpenPage(ctx, entry);
    return true;
}

bool OwnsScreen(uint32_t screen) {
    if (!screen || screen != g_state.screen || g_layout.menu < 0) return false;
    return R32(screen + kScreenTakesInput) == 1 && R32(screen + kScreenBlocked) == 0;
}

// true when the options menu dealt with a button the screen got; a bind waiting for its key keeps them all
bool TakeButton(X86Context& ctx, uint32_t screen, uint32_t code) {
    if (!OwnsScreen(screen)) return false;
    if (app::Capturing()) return true;
    return PageOpen() ? TakePageButton(ctx, code) : TakeListButton(ctx, code);
}

// a held direction on a page: the screen's own timing, the page's own step
bool TakeHeldButton(X86Context& ctx, uint32_t screen, uint32_t code) {
    if (!OwnsScreen(screen) || !PageOpen()) return false;
    if (app::Capturing()) return true;
    bool direction = code >= kButtonLeft && code <= kButtonDown;
    if (!direction) return false;
    if (R32(screen + kScreenHeldCode) == kNoCode) W32(screen + kScreenHeldCode, code);
    uint32_t held = R32(screen + kScreenHeldCode);
    float& wait = G<float>(screen + kScreenHeldWait);
    if (code == held) wait -= G<float>(kFrameSeconds);
    if (wait > 0.0f) return true;
    TakeDirection(ctx, held);
    wait = G<float>(kHeldDelay);
    return true;
}

// Finding the menu.

bool Matches(uint32_t object, bool input) {
    if (R32(object) != kMenuScreenVtable) return false;
    if (input && R32(object + kScreenTakesInput) != 1) return false;
    if (R32(object + kScreenColumns) != 1) return false;
    int rows = int(R32(object + kScreenRows));
    bool ours = PageOpen() && rows == PageSlots();
    return rows == g_layout.stock || rows == g_layout.stock + EntryCount() || ours;
}

// the newest open menu screen with the rows the menu drew
uint32_t FindScreen(bool input = true) {
    uint32_t list = R32(kObjects);
    for (uint32_t i = R32(kObjectCount); i > 0; i--) {
        uint32_t object = R32(list + (i - 1) * 4);
        if (object && Matches(object, input)) return object;
    }
    return 0;
}

int SeenCount(uint32_t labels) {
    int count = 0;
    for (uint32_t bits = labels; bits; bits &= bits - 1) count++;
    return count;
}

// the rows of a menu whose labels all showed, ordered top to bottom
Layout MakeLayout(const Sight& sight) {
    Layout layout;
    if (sight.menu < 0) return layout;
    const Menu& menu = kMenus[sight.menu];
    if (SeenCount(sight.labels) != menu.count) return layout;
    std::array<int, kMaxRows> order{};
    for (int i = 0; i < menu.count; i++) order[size_t(i)] = i;
    auto higher = [&](int a, int b) { return sight.place[size_t(a)].y < sight.place[size_t(b)].y; };
    std::sort(order.begin(), order.begin() + menu.count, higher);
    for (int row = 0; row < menu.count; row++) {
        int label = order[size_t(row)];
        layout.row[size_t(row)] = sight.place[size_t(label)];
        layout.row_of[size_t(label)] = row;
    }
    layout.menu = sight.menu;
    layout.stock = menu.count;
    layout.spacing = {layout.row[1].x - layout.row[0].x, layout.row[1].y - layout.row[0].y};
    layout.text = sight.text;
    return layout;
}

// the list scrolls only when the cursor leaves the rows on show
void UpdateListTop() {
    int index = CursorIndex();
    int& top = g_state.list_top;
    if (PageOpen() || index < 0) return;
    if (index < top) top = index;
    if (index >= top + ListSlots()) top = index - ListSlots() + 1;
}

void UpdateScreen() {
    uint32_t screen = FindScreen();
    if (screen != g_state.screen) {
        g_state = {};
        g_state.screen = screen;
    }
    if (!screen) return;
    int rows = WantedRows();
    if (int(R32(screen + kScreenRows)) != rows) SetRows(rows);
    UpdateListTop();
}

// Seeing the stock rows.

int LabelIndex(const char* raw, int* menu) {
    for (int m = 0; m < int(std::size(kMenus)); m++) {
        for (int i = 0; i < kMenus[m].count; i++) {
            if (std::strncmp(raw, kMenus[m].labels[i], kMaxRaw) != 0) continue;
            *menu = m;
            return i;
        }
    }
    return -1;
}

// a label of the frame's menu, recorded for the next frame's layout
void LatchLabel(int menu, int label, Place place) {
    if (g_now.menu >= 0 && g_now.menu != menu) return;
    g_now.menu = menu;
    if (g_now.labels & (1u << label)) return;
    g_now.labels |= 1u << label;
    g_now.place[size_t(label)] = place;
}

// a field level with a stock row and right of its label is that row's value
int ValueRow(Place place) {
    float band = std::fabs(g_layout.spacing.y) * 0.5f;
    for (int row = 0; row < g_layout.stock; row++) {
        Place label = g_layout.row[size_t(row)];
        if (std::fabs(place.y - label.y) < band && place.x > label.x) return row;
    }
    return -1;
}

// the slots' height on screen, where a page hides every text the stock rows show
bool InRows(Place place) {
    if (g_layout.menu < 0) return false;
    float half = std::fabs(g_layout.spacing.y) * 0.5f;
    float first = SlotPlace(0).y;
    float last = SlotPlace(Slots() - 1).y;
    return place.y > std::min(first, last) - half && place.y < std::max(first, last) + half;
}

void ReadCxform(Cxform& to) {
    for (int i = 0; i < kCxformWords; i++) {
        to[size_t(i)] = R32(kCxformMul + uint32_t(i) * 4);
        to[size_t(kCxformWords + i)] = R32(kCxformAdd + uint32_t(i) * 4);
    }
}

void WriteCxform(const Cxform& from) {
    for (int i = 0; i < kCxformWords; i++) {
        W32(kCxformMul + uint32_t(i) * 4, from[size_t(i)]);
        W32(kCxformAdd + uint32_t(i) * 4, from[size_t(kCxformWords + i)]);
    }
}

void LatchText(uint32_t data, const Field& field) {
    uint32_t shown = R32(data + kFieldShown);
    if (!shown) shown = R32(data + kFieldRaw);
    const char* text = shown ? static_cast<const char*>(HostPtr(shown)) : "";
    g_now.text[size_t(field.row)][field.part] = std::string(text, strnlen(text, kTextBytes));
}

void LatchLook(uint32_t data, const Field& field) {
    if (!g_state.screen || field.order >= kMaxFields) return;
    bool active = field.row == CursorIndex();
    if (!active && field.row != SourceRow()) return;
    Look& look = g_looks[field.part][active][field.order];
    look.colour = R32(data + kFieldColour);
    ReadCxform(look.cxform);
    look.seen = true;
}

// a stock row's field: its row by the last frame's layout
std::optional<Field> Observe(uint32_t data, Place place) {
    uint32_t raw = R32(data + kFieldRaw);
    if (!raw) return std::nullopt;
    int menu = -1;
    int label = LabelIndex(static_cast<const char*>(HostPtr(raw)), &menu);
    if (label >= 0) LatchLabel(menu, label, place);
    if (g_layout.menu < 0) return std::nullopt;
    Field field{-1, kLabel, 0};
    if (label >= 0 && menu == g_layout.menu) {
        field.row = g_layout.row_of[size_t(label)];
    } else if (label < 0) {
        field = {ValueRow(place), kValue, 0};
    }
    if (field.row < 0) return std::nullopt;
    field.order = g_order[size_t(field.row)][field.part]++;
    if (field.order == 0) LatchText(data, field);
    LatchLook(data, field);
    return field;
}

// Drawing copies of a stock row.

// moves and scales the drawn character for one draw
class MatrixShift {
public:
    MatrixShift(Place by, float scale) : matrix_(R32(R32(kRenderer) + kRendererMatrix)) {
        for (size_t i = 0; i < kSaved.size(); i++) saved_[i] = G<float>(matrix_ + kSaved[i]);
        G<float>(matrix_ + kMatrixX) += by.x;
        G<float>(matrix_ + kMatrixY) += by.y;
        G<float>(matrix_ + kMatrixScaleX) *= scale;
        G<float>(matrix_ + kMatrixScaleY) *= scale;
    }
    ~MatrixShift() {
        for (size_t i = 0; i < kSaved.size(); i++) G<float>(matrix_ + kSaved[i]) = saved_[i];
    }

private:
    static constexpr std::array<uint32_t, 4> kSaved = {kMatrixX, kMatrixY, kMatrixScaleX, kMatrixScaleY};
    uint32_t matrix_;
    std::array<float, 4> saved_{};
};

// sets the colour transform for one draw
class CxformSwap {
public:
    explicit CxformSwap(const Look& look) : active_(look.seen) {
        if (!active_) return;
        ReadCxform(saved_);
        WriteCxform(look.cxform);
    }
    ~CxformSwap() {
        if (active_) WriteCxform(saved_);
    }

private:
    bool active_;
    Cxform saved_{};
};


void WriteText(uint32_t at, const std::string& text) {
    size_t size = std::min(text.size(), size_t(kTextBytes - 1));
    std::memcpy(HostPtr(at), text.data(), size);
    G<char>(at + uint32_t(size)) = 0;
}

// the field's render data with another text
uint32_t CopyField(uint32_t field, const std::string& text) {
    static const uint32_t data = mem::AllocSystem(kFieldBytes + kTextBytes);
    std::memcpy(HostPtr(data), HostPtr(field), kFieldBytes);
    WriteText(data + kFieldBytes, text);
    W32(data + kFieldShown, data + kFieldBytes);
    return data;
}

// the box grows away from the side the text keeps to
void Widen(uint32_t data) {
    float& left = G<float>(data + kFieldLeft);
    float& right = G<float>(data + kFieldRight);
    float grow = (right - left) * kWiden;
    uint32_t align = R32(data + kFieldAlign);
    if (align != kAlignRight) right += grow;
    if (align != kAlignLeft) left -= grow;
}

bool IsRaw(uint32_t field, const char* text) {
    uint32_t raw = R32(field + kFieldRaw);
    return raw && std::strncmp(static_cast<const char*>(HostPtr(raw)), text, kMaxRaw) == 0;
}

// the field drawn with another text, its box widened so the text keeps one line
void DrawRelabelled(X86Context& ctx, uint32_t field, const std::string& text) {
    uint32_t data = CopyField(field, text);
    Widen(data);
    X86Context saved = ctx;
    g_copying = true;
    CallGuest(ctx, kDrawField, {data, 0});
    g_copying = false;
    ctx = saved;
}

// a copy in the look of the stock row's field, active or not, moved and scaled
void DrawCopy(X86Context& ctx, uint32_t data, const Field& seen, bool active, Place by, float scale) {
    const Look& look = g_looks[seen.part][active][seen.order];
    if (look.seen) W32(data + kFieldColour, look.colour);
    CxformSwap swap(look);
    MatrixShift shift(by, scale);
    g_copying = true;
    CallGuest(ctx, kDrawField, {data, 0});
    g_copying = false;
}

// the scrolled list: the stock rows' own texts moved up a slot each, then the entries
std::string ListText(int row, Part part) {
    if (row < g_layout.stock) return g_layout.text[size_t(row)][part];
    return part == kLabel ? MenuText(kEntries[row - g_layout.stock]) : "";
}

void LatchLines(uint32_t field);

void DrawList(X86Context& ctx, uint32_t field, const Field& seen) {
    int index = CursorIndex();
    for (int slot = 0; slot < ListSlots(); slot++) {
        int row = g_state.list_top + slot;
        std::string text = ListText(row, seen.part);
        if (text.empty()) continue;
        uint32_t data = CopyField(field, text);
        Widen(data);
        // the rows share a column, centred texts keep their own x
        Place by{0.0f, LineY(slot) - g_layout.row[size_t(seen.row)].y};
        DrawCopy(ctx, data, seen, index == row, by, 1.0f);
    }
    if (seen.order == 0) LatchLines(field);
}

float Middle(uint32_t data, uint32_t low, uint32_t high) { return (G<float>(data + low) + G<float>(data + high)) / 2; }

float MatrixValue(uint32_t offset) { return G<float>(R32(R32(kRenderer) + kRendererMatrix) + offset); }

// one side of a page line: the box spans the column, the text scaled about its middle so the line keeps its place
void DrawLineSide(X86Context& ctx, uint32_t field, const Field& seen, const std::string& text, int slot, bool active,
                  uint32_t align) {
    float scale_x = MatrixValue(kMatrixScaleX);
    float scale_y = MatrixValue(kMatrixScaleY);
    if (text.empty() || !(scale_x > 0.0f)) return;
    uint32_t data = CopyField(field, text);
    float middle_x = Middle(data, kFieldLeft, kFieldRight);
    float middle_y = Middle(data, kFieldTop, kFieldBottom);
    float half = kColumnHalf / (scale_x * kPageTextScale);
    G<float>(data + kFieldLeft) = middle_x - half;
    G<float>(data + kFieldRight) = middle_x + half;
    W32(data + kFieldAlign, align);
    float kept = 1.0f - kPageTextScale;
    float line_dy = LineY(slot) - g_layout.row[size_t(seen.row)].y;
    Place by{middle_x * scale_x * kept, line_dy + middle_y * scale_y * kept};
    DrawCopy(ctx, data, seen, active, by, kPageTextScale);
}

// where the lines are, for the mouse
void LatchLines(uint32_t field) {
    float middle_x = Middle(field, kFieldLeft, kFieldRight);
    float middle_y = Middle(field, kFieldTop, kFieldBottom);
    g_lines.middle_x = MatrixValue(kMatrixX) + middle_x * MatrixValue(kMatrixScaleX);
    g_lines.middle_dy = middle_y * MatrixValue(kMatrixScaleY);
    g_lines.seen = true;
}

// the rows from the one at top, one a line; the cursor's line, -1 for none, shows active
void DrawLines(X86Context& ctx, uint32_t field, const Field& seen, const std::vector<const Row*>& rows, int top,
               int cursor) {
    if (seen.part != kLabel) return;
    int lines = std::min(int(rows.size()) - top, LineSlots());
    for (int slot = 0; slot < lines; slot++) {
        const Row* row = rows[size_t(top + slot)];
        bool active = cursor == slot;
        DrawLineSide(ctx, field, seen, RowLabel(row), slot, active, kAlignLeft);
        DrawLineSide(ctx, field, seen, RowValue(row), slot, active, kAlignRight);
    }
    if (seen.order == 0) LatchLines(field);
}

void DrawCopies(X86Context& ctx, uint32_t field, const Field& seen) {
    if (seen.row != SourceRow() || seen.order >= kMaxFields) return;
    if (int preview = PreviewPage(); preview >= 0) {
        DrawLines(ctx, field, seen, PageList(preview, nullptr), 0, -1);
    } else if (!g_state.screen) {
        return;
    } else if (PageOpen()) {
        DrawLines(ctx, field, seen, g_state.rows, g_state.top, CursorIndex());
    } else if (ListShown()) {
        DrawList(ctx, field, seen);
    }
}

// Hiding what a stock row holds besides its label.

bool IsNamed(uint32_t clip, const char* name) {
    uint32_t text = R32(clip + flash::kClipName);
    return flash::Readable(text) && _stricmp(flash::Text(text + flash::kStringText), name) == 0;
}

// a stock label field in the clip or below it
bool HoldsLabel(uint32_t clip, int levels) {
    if (uint32_t field = flash::EditTextField(clip)) {
        uint32_t raw = R32(field + kFieldRaw);
        int menu = -1;
        return flash::Readable(raw) && LabelIndex(flash::Text(raw), &menu) >= 0;
    }
    if (levels == 0) return false;
    for (uint32_t child : flash::Children(clip)) {
        if (HoldsLabel(child, levels - 1)) return true;
    }
    return false;
}

void LatchCursorRow() {
    g_frame++;
    int index = StockFocus();
    if (index == g_last_index) return;
    for (int row : {g_last_index, index}) {
        if (row >= 0 && row < kMaxRows) g_changed[size_t(row)] = g_frame;
    }
    g_last_index = index;
}

}  // namespace

// once a frame, at the frame's end: the next frame's rows come from what this one drew
void LatchOptionsMenu(X86Context&, uint8_t*) {
    // read every frame, so a Delete that answered a key capture is not a fresh press once it ends
    bool unbind = input::TakeDeletePress();
    if (unbind && PageOpen() && OwnsScreen(g_state.screen) && !input::CaptureRecent()) UnbindFocusedRow();
    g_layout = MakeLayout(g_now);
    g_now = {};
    g_order = {};
    if (g_layout.menu >= 0) {
        g_state.missing = 0;
        UpdateScreen();
        g_panel = g_state.screen ? g_state.screen : FindScreen(false);
        LatchCursorRow();
    } else if (++g_state.missing > kForgetFrames) {
        g_state = {};
    }
}

int OptionsMenuStockRows(uint32_t screen) {
    bool ours = screen && screen == g_state.screen && g_layout.menu >= 0;
    return ours ? g_layout.stock : 0;
}

int OptionsMenuIndexAt(int slot) { return PageOpen() ? slot : g_state.list_top + slot; }

void OpenOptionsPage(int page) { g_tab_page = page; }

// in the pad's turn, so the page's first focus is the screens' own
void OptionsMenuFocus(X86Context& ctx) {
    bool pause = g_layout.menu == kPause && OwnsScreen(g_state.screen);
    if (g_tab_page < 0 || !pause || PageOpen()) return;
    OpenPage(ctx, g_tab_page);
    g_state.direct = true;
    g_tab_page = -1;
}

// on a page: a clip inside a pause menu row that holds no label, its value box or arrows
bool HideOptionsClip(uint32_t clip) {
    bool page = PageOpen() || PreviewPage() >= 0;
    if (!page || flash::Type(clip) == flash::kTypeEditText) return false;
    uint32_t parent = R32(clip + flash::kClipParent);
    for (int level = 0; flash::Readable(parent) && level < kMaxPanelLevels; level++) {
        // a row is the panel's child, what it holds is two levels under the panel
        if (level > 0 && IsNamed(parent, kOptionsPanel)) return !HoldsLabel(clip, kLabelLevels);
        parent = R32(parent + flash::kClipParent);
    }
    return false;
}

std::optional<int> OptionsMenuRowAt(uint32_t screen, float stage_x, float stage_y) {
    bool lines = PageOpen() || ListShown();
    if (!lines || screen != g_state.screen || !g_lines.seen) return std::nullopt;
    float width = G<float>(kStageWidthNow);
    float height = G<float>(kStageHeightNow);
    if (!(width > 0.0f) || !(height > 0.0f)) return -1;
    float x = stage_x * kStageWidth / width;
    float y = stage_y * kStageHeight / height;
    if (std::fabs(x - g_lines.middle_x) > kColumnHalf) return -1;
    float step = LineStep() > 0.0f ? LineStep() : kColumnHalf;
    int slots = PageOpen() ? PageSlots() : ListSlots();
    int top = PageOpen() ? 0 : g_state.list_top;
    for (int slot = 0; slot < slots; slot++) {
        if (std::fabs(y - LineY(slot) - g_lines.middle_dy) <= step / 2) return top + slot;
    }
    return -1;
}

// the Apt text field draw, cdecl (field, unused): a page hides the stock rows and draws its own
X86_EXTERN_FUNC(__imp__sub_00413E50);
static void DrawField(X86Context& ctx, uint8_t* base) {
    if (g_copying) return __imp__sub_00413E50(ctx, base);
    uint32_t data = R32(ctx.esp + 4);
    const char* label = PauseTabLabel();
    if (!label && !DrawingPauseTab() && IsRaw(data, kPauseTitle)) label = PauseTabTitle();
    if (label) {
        DrawRelabelled(ctx, data, label);
        Return(ctx, 0, 0);
        return;
    }
    Place place = DrawnPlace();
    std::optional<Field> seen = Observe(data, place);
    bool hiding = PageOpen() || ListShown() || PreviewPage() >= 0;
    if (hiding && !DrawingPauseTab() && InRows(place)) {
        Return(ctx, 0, 0);
    } else {
        __imp__sub_00413E50(ctx, base);
    }
    if (!seen) return;
    X86Context saved = ctx;
    DrawCopies(ctx, data, *seen);
    ctx = saved;
}

X86_FUNC(sub_00413E50) {
    SetPromptField(R32(ctx.esp + 4));
    DrawField(ctx, base);
    SetPromptField(0);
}

// a menu screen takes a button, thiscall (port, button)
X86_EXTERN_FUNC(__imp__sub_0001DA08);
X86_FUNC(sub_0001DA08) {
    X86Context saved = ctx;
    uint32_t screen = ctx.ecx;
    uint32_t code = R32(ctx.esp + 8);
    bool taken = TakePauseTabButton(ctx, screen, code) || TakeButton(ctx, screen, code);
    ctx = saved;
    if (taken) {
        Return(ctx, 0, 2);
    } else {
        __imp__sub_0001DA08(ctx, base);
    }
}

// a menu screen steps a held direction, thiscall (port, button): a page repeats its own steps
X86_EXTERN_FUNC(__imp__sub_00011299);
X86_FUNC(sub_00011299) {
    X86Context saved = ctx;
    uint32_t screen = ctx.ecx;
    uint32_t code = R32(ctx.esp + 8);
    bool taken = HoldPauseTabButton(screen) || TakeHeldButton(ctx, screen, code);
    ctx = saved;
    if (taken) {
        Return(ctx, 0, 2);
    } else {
        __imp__sub_00011299(ctx, base);
    }
}
