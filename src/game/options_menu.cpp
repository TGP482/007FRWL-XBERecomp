#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "common/menu.h"
#include "game/options_menu.h"
#include "core/log.h"
#include "xdk/hle.h"

// Recomp settings in the game's own options menu, the front end's and the pause menu's. Entries under the stock rows
// open pages with the F10 panel's rows. Each row is drawn as a copy of a stock row's text, and the stock menu screen
// keeps the cursor, so the rows look and sound as the game's own.

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

// button codes the screens get: the row goes up and down, the column left and right
constexpr uint32_t kButtonUp = 2;
constexpr uint32_t kButtonDown = 3;
constexpr uint32_t kButtonLeft = 4;
constexpr uint32_t kButtonRight = 5;

// every live UI object, the menu screens among them
constexpr uint32_t kObjects = 0x8375E4;
constexpr uint32_t kObjectCount = 0x8375E8;

// Apt text field render data, made at 0x413D80 and drawn by kDrawField
constexpr uint32_t kFieldLeft = 0x04;  // the box, floats in the field's own space
constexpr uint32_t kFieldRight = 0x0C;
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
constexpr uint32_t kRendererMatrix = 4;
constexpr uint32_t kMatrixX = 0x30;
constexpr uint32_t kMatrixY = 0x34;
// colour transform of the drawn character: four multipliers, four offsets
constexpr uint32_t kCxformMul = 0x837D94;
constexpr uint32_t kCxformAdd = 0x837D30;
constexpr int kCxformWords = 4;

// seconds of the frame, as the screens step a held direction
constexpr uint32_t kFrameSeconds = 0x824358;
constexpr float kFirstRepeat = 0.5f;  // the screens' own delays
constexpr float kRepeat = 0.1f;

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
    const char* name;
    const char* const* labels;
    int count;
};
const Menu kMenus[] = {
    {"front end", kFrontEndRows, int(std::size(kFrontEndRows))},
    {"pause", kPauseRows, int(std::size(kPauseRows))},
};
constexpr int kMaxRows = 8;
constexpr size_t kMaxRaw = 64;

// an entry's page is rows::Page of its index
const char* const kEntries[] = {"RECOMP SETTINGS", "MOUSE AND KEYBOARD"};
constexpr int kEntryCount = int(std::size(kEntries));
const char* const kDefaultsLabel = "RESTORE DEFAULTS";

constexpr int kForgetFrames = 15;  // a menu not drawn this many frames is closed
constexpr uint32_t kTextBytes = 128;
constexpr int kMaxFields = 4;     // text fields of one part of a row, shadows included
constexpr float kWiden = 3.0f;    // a copy's box grows by this many of its widths, so a long text keeps one line

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
    bool value = false;  // a field right of a label
    std::array<std::array<std::string, kPartCount>, kMaxRows> text{};  // by the last frame's rows
};

// the stock rows of the last frame in screen order
struct Layout {
    int menu = -1;
    int stock = 0;
    std::array<Place, kMaxRows> row{};
    std::array<int, kMaxRows> row_of{};  // the row of each label
    Place spacing{};
    bool value = false;
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

// a held direction repeats as on the screens
struct Repeat {
    uint32_t code = UINT32_MAX;
    float wait = 0.0f;
};

// the menu screen the rows belong to
struct State {
    uint32_t screen = 0;
    int page = -1;  // the open page, -1 while the list shows
    int top = 0;    // the page row in the first slot
    int list_top = 0;  // the list row in the first slot, past 0 once the cursor goes below the stock rows
    Repeat repeat;
    int missing = 0;  // frames the menu was not drawn
};

// all on the game thread
Sight g_now;
Layout g_layout;
State g_state;
std::array<std::array<int, kPartCount>, kMaxRows> g_order{};
Look g_looks[kPartCount][2][kMaxFields];
bool g_copying = false;

bool PageOpen() { return g_state.page >= 0; }

int CursorIndex() { return g_state.screen ? int(R32(g_state.screen + kScreenIndex)) : -1; }

// rows show in the stock rows' places, scrolling past them
int Slots() { return g_layout.stock; }

Place DrawnPlace() {
    uint32_t matrix = R32(R32(kRenderer) + kRendererMatrix);
    return {G<float>(matrix + kMatrixX), G<float>(matrix + kMatrixY)};
}

Place SlotPlace(int slot) { return g_layout.row[size_t(std::clamp(slot, 0, g_layout.stock - 1))]; }

bool ListScrolled() { return !PageOpen() && g_state.list_top > 0; }

// Pages.

// the page's rows without headings; one more row after them restores the defaults
const std::vector<const Row*>& PageRows(int page) {
    static std::vector<const Row*> lists[kEntryCount];
    std::vector<const Row*>& list = lists[page];
    if (!list.empty()) return list;
    size_t count = 0;
    const Row* rows = app::rows::Page(page, &count);
    for (size_t i = 0; i < count; i++) {
        if (rows[i].kind != Kind::Heading) list.push_back(&rows[i]);
    }
    return list;
}

int PageRowCount() { return int(PageRows(g_state.page).size()) + 1; }

// the slots the page shows at once, the screen's rows
int PageSlots() { return std::min(PageRowCount(), Slots()); }

const Row* PageRow(int index) {
    const std::vector<const Row*>& rows = PageRows(g_state.page);
    return index >= 0 && index < int(rows.size()) ? rows[size_t(index)] : nullptr;
}

std::string RowLabel(int index) {
    const Row* row = PageRow(index);
    return row ? row->label : kDefaultsLabel;
}

std::string RowValue(int index) {
    const Row* row = PageRow(index);
    return row ? app::rows::Value(*row) : std::string();
}

// The screen.

int WantedRows() { return PageOpen() ? PageSlots() : g_layout.stock + kEntryCount; }

void SetRows(int rows) {
    W32(g_state.screen + kScreenRows, uint32_t(rows));
    W32(g_state.screen + kScreenCount, uint32_t(rows));
}

// as the screen's own input moves it: OnDeselect and OnSelect follow
void MoveCursor(X86Context& ctx, int index) {
    W32(g_state.screen + kScreenRow, uint32_t(index));
    W32(g_state.screen + kScreenColumn, 0);
    ctx.ecx = g_state.screen;
    CallGuest(ctx, kScreenSelect, {});
}

void OpenPage(X86Context& ctx, int page) {
    g_state.page = page;
    g_state.top = 0;
    SetRows(PageSlots());
    MoveCursor(ctx, 0);
}

void ClosePage(X86Context& ctx) {
    int entry = g_state.page;
    g_state.page = -1;
    SetRows(g_layout.stock + kEntryCount);
    MoveCursor(ctx, g_layout.stock + entry);
}

// up and down move the cursor over the slots and scroll the page at their ends
void MovePageCursor(X86Context& ctx, int step) {
    int slots = PageSlots();
    int last_top = PageRowCount() - slots;
    int slot = CursorIndex() + step;
    if (slot < 0 && g_state.top > 0) {
        g_state.top--;
    } else if (slot >= slots && g_state.top < last_top) {
        g_state.top++;
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

void StepRow(int step) {
    const Row* row = PageRow(g_state.top + CursorIndex());
    if (row && row->kind == Kind::Value) app::rows::Value(*row, step);
}

void ActivateRow() {
    const Row* row = PageRow(g_state.top + CursorIndex());
    if (!row) {
        app::rows::Defaults(g_state.page);
    } else if (row->kind == Kind::Value) {
        app::rows::Value(*row, 1);
    } else {
        app::rows::Bind(*row);
    }
}

// a direction on a page, pressed or repeating
bool TakeDirection(X86Context& ctx, uint32_t code) {
    if (code == kButtonUp || code == kButtonDown) {
        MovePageCursor(ctx, code == kButtonUp ? -1 : 1);
    } else if (code == kButtonLeft || code == kButtonRight) {
        StepRow(code == kButtonLeft ? -1 : 1);
    } else {
        return false;
    }
    return true;
}

// a button on a page: the page keeps every button the screen would act on
bool TakePageButton(X86Context& ctx, uint32_t code) {
    if (code == R32(g_state.screen + kScreenCancel)) {
        ClosePage(ctx);
        return true;
    }
    if (code == R32(g_state.screen + kScreenAccept)) {
        ActivateRow();
        return true;
    }
    if (!TakeDirection(ctx, code)) return false;
    g_state.repeat = {code, kFirstRepeat};
    return true;
}

// a button on the list: the entries open their page
bool OnNfsDemo() { return g_layout.menu == kFrontEnd && CursorIndex() == g_layout.row_of[kNfsDemoLabel]; }

bool TakeListButton(X86Context& ctx, uint32_t code) {
    bool accept = code == R32(g_state.screen + kScreenAccept);
    if (accept && OnNfsDemo()) return true;
    int entry = CursorIndex() - g_layout.stock;
    bool on_entry = entry >= 0 && entry < kEntryCount;
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

// a held direction on a page steps again after the screens' delays
bool TakeHeldButton(X86Context& ctx, uint32_t screen, uint32_t code) {
    if (!OwnsScreen(screen) || !PageOpen()) return false;
    if (app::Capturing()) return true;
    Repeat& repeat = g_state.repeat;
    bool direction = code >= kButtonUp && code <= kButtonRight;
    if (!direction) return false;
    if (code != repeat.code) return true;
    repeat.wait -= G<float>(kFrameSeconds);
    if (repeat.wait > 0.0f) return true;
    repeat.wait = kRepeat;
    TakeDirection(ctx, code);
    return true;
}

// Finding the menu.

bool Matches(uint32_t object) {
    if (R32(object) != kMenuScreenVtable || R32(object + kScreenTakesInput) != 1) return false;
    if (R32(object + kScreenColumns) != 1) return false;
    int rows = int(R32(object + kScreenRows));
    bool ours = PageOpen() && rows == PageSlots();
    return rows == g_layout.stock || rows == g_layout.stock + kEntryCount || ours;
}

// the newest open menu screen with the rows the menu drew
uint32_t FindScreen() {
    uint32_t list = R32(kObjects);
    for (uint32_t i = R32(kObjectCount); i > 0; i--) {
        uint32_t object = R32(list + (i - 1) * 4);
        if (object && Matches(object)) return object;
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
    layout.value = sight.value;
    layout.text = sight.text;
    return layout;
}

void Forget() {
    if (g_state.screen) LOG_FIRST(8, "options menu: closed");
    g_state = {};
}

// the list scrolls only when the cursor leaves the rows on show
void UpdateListTop() {
    int index = CursorIndex();
    int& top = g_state.list_top;
    if (PageOpen() || index < 0) return;
    if (index < top) top = index;
    if (index >= top + Slots()) top = index - Slots() + 1;
}

void UpdateScreen() {
    uint32_t screen = FindScreen();
    if (screen != g_state.screen) {
        LOG_FIRST(8, "options menu: %s, %d rows, screen %08X", kMenus[g_layout.menu].name, g_layout.stock, screen);
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
        g_now.value = g_now.value || field.row >= 0;
    }
    if (field.row < 0) return std::nullopt;
    field.order = g_order[size_t(field.row)][field.part]++;
    if (field.order == 0) LatchText(data, field);
    LatchLook(data, field);
    return field;
}

// Drawing copies of a stock row.

// what a copy shows and where
struct Copy {
    std::string text;
    int slot;
    bool active;
};

// moves the drawn character for one draw
class MatrixShift {
public:
    explicit MatrixShift(Place by) : matrix_(R32(R32(kRenderer) + kRendererMatrix)) {
        saved_ = {G<float>(matrix_ + kMatrixX), G<float>(matrix_ + kMatrixY)};
        G<float>(matrix_ + kMatrixX) = saved_.x + by.x;
        G<float>(matrix_ + kMatrixY) = saved_.y + by.y;
    }
    ~MatrixShift() {
        G<float>(matrix_ + kMatrixX) = saved_.x;
        G<float>(matrix_ + kMatrixY) = saved_.y;
    }

private:
    uint32_t matrix_;
    Place saved_;
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

// the stock row copies are made from: the first one the cursor is not on
int SourceRow() { return CursorIndex() == 0 ? 1 : 0; }

void WriteText(uint32_t at, const std::string& text) {
    size_t size = std::min(text.size(), size_t(kTextBytes - 1));
    std::memcpy(HostPtr(at), text.data(), size);
    G<char>(at + uint32_t(size)) = 0;
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

// a copy of the field being drawn, with its own text, look and place
void DrawCopy(X86Context& ctx, uint32_t field, const Field& seen, const Copy& copy) {
    static const uint32_t data = mem::AllocSystem(kFieldBytes + kTextBytes);
    if (copy.text.empty() || seen.order >= kMaxFields) return;
    uint32_t text = data + kFieldBytes;
    std::memcpy(HostPtr(data), HostPtr(field), kFieldBytes);
    WriteText(text, copy.text);
    W32(data + kFieldShown, text);
    Widen(data);
    const Look& look = g_looks[seen.part][copy.active][seen.order];
    if (look.seen) W32(data + kFieldColour, look.colour);
    CxformSwap swap(look);
    Place to = SlotPlace(copy.slot);
    Place from = g_layout.row[size_t(seen.row)];
    MatrixShift shift({0.0f, to.y - from.y});  // the rows share a column, centred texts keep their own x
    g_copying = true;
    CallGuest(ctx, kDrawField, {data, 0});
    g_copying = false;
}

// the scrolled list: the stock rows' own texts moved up a slot each, then the entries
std::string ListText(int row, Part part) {
    if (row < g_layout.stock) return g_layout.text[size_t(row)][part];
    return part == kLabel ? kEntries[row - g_layout.stock] : "";
}

void DrawList(X86Context& ctx, uint32_t field, const Field& seen) {
    int index = CursorIndex();
    for (int slot = 0; slot < Slots(); slot++) {
        int row = g_state.list_top + slot;
        DrawCopy(ctx, field, seen, {ListText(row, seen.part), slot, index == row});
    }
}

// the value in the slot's own value box where the stock row there has one, else after the label
std::string PageText(int row, int slot, Part part) {
    bool box = !g_layout.text[size_t(slot)][kValue].empty();
    std::string value = RowValue(row);
    if (part == kValue) return box ? value : "";
    std::string label = RowLabel(row);
    if (box || value.empty()) return label;
    return label + ": " + value;
}

void DrawPage(X86Context& ctx, uint32_t field, const Field& seen) {
    int index = CursorIndex();
    for (int slot = 0; slot < PageSlots(); slot++) {
        std::string text = PageText(g_state.top + slot, slot, seen.part);
        DrawCopy(ctx, field, seen, {text, slot, index == slot});
    }
}

void DrawCopies(X86Context& ctx, uint32_t field, const Field& seen) {
    if (!g_state.screen || seen.row != SourceRow()) return;
    if (PageOpen()) {
        DrawPage(ctx, field, seen);
    } else if (ListScrolled()) {
        DrawList(ctx, field, seen);
    }
}

}  // namespace

// once a frame, at the frame's end: the next frame's rows come from what this one drew
void LatchOptionsMenu(X86Context&, uint8_t*) {
    g_layout = MakeLayout(g_now);
    g_now = {};
    g_order = {};
    if (g_layout.menu >= 0) {
        g_state.missing = 0;
        UpdateScreen();
    } else if (++g_state.missing > kForgetFrames) {
        Forget();
    }
}

int OptionsMenuStockRows(uint32_t screen) {
    bool ours = screen && screen == g_state.screen && g_layout.menu >= 0;
    return ours ? g_layout.stock : 0;
}

int OptionsMenuIndexAt(int slot) { return PageOpen() ? slot : g_state.list_top + slot; }

// the Apt text field draw, cdecl (field, unused): a page hides the stock rows and draws its own
X86_EXTERN_FUNC(__imp__sub_00413E50);
X86_FUNC(sub_00413E50) {
    if (g_copying) return __imp__sub_00413E50(ctx, base);
    uint32_t data = R32(ctx.esp + 4);
    Place place = DrawnPlace();
    std::optional<Field> seen = Observe(data, place);
    if ((PageOpen() || ListScrolled()) && InRows(place)) {
        Return(ctx, 0, 0);
    } else {
        __imp__sub_00413E50(ctx, base);
    }
    if (!seen) return;
    X86Context saved = ctx;
    DrawCopies(ctx, data, *seen);
    ctx = saved;
}

// a menu screen takes a button, thiscall (port, button)
X86_EXTERN_FUNC(__imp__sub_0001DA08);
X86_FUNC(sub_0001DA08) {
    X86Context saved = ctx;
    bool taken = TakeButton(ctx, ctx.ecx, R32(ctx.esp + 8));
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
    bool taken = TakeHeldButton(ctx, ctx.ecx, R32(ctx.esp + 8));
    ctx = saved;
    if (taken) {
        Return(ctx, 0, 2);
    } else {
        __imp__sub_00011299(ctx, base);
    }
}
