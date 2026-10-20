// Autosave. The save boxes (the trc page's save child, save_mc) are never drawn and are answered YES by the box's own
// script, so the game saves exactly as on the console. The stats screen opens its box as soon as it takes input and
// stays up after the save; its continue then leaves without a box. A box asking anything else, such as a full memory
// unit, is shown. "AUTOSAVING..." shows top right while a box is up.
#include <cstring>
#include <optional>
#include <string>

#include "common/options.h"
#include "game/flash.h"
#include "game/hud.h"
#include "game/menu_ports.h"
#include "game/options_menu.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace flash;

namespace {

// menu screen
constexpr uint32_t kScreenListener = 0x6C;
constexpr uint32_t kListenerHeld = 0x14;
constexpr uint32_t kScreenScript = 0x88;
constexpr uint32_t kScreenBlocked = 0xDC;  // what the script's BlockInput writes
constexpr uint32_t kScreenActive = 0xE0;

// screen script handler, vtable 0x6B1C44: each slot calls the page script's function of that name
constexpr uint32_t kScriptOnActivate = 0x10;  // thiscall (index)
constexpr uint32_t kYesRow = 0;               // the box's rows: YES, NO, CANCEL
constexpr uint32_t kOnCloseChild = 0x14D2C;   // slot 0x18, fastcall on the handler

// clips of the box and of the stats screen
constexpr const char* kSaveBoxClip = "save_mc";
constexpr const char* kChoiceClip = "choice_mc";  // a question from the save system, not the box's own
constexpr const char* kStatsTitle = "level_name";
constexpr int kMaxParents = 32;
constexpr int kTitleDepth = 3;

// save system: its messages table holds TRC_XBOX_02 "Are you sure you want to overwrite this saved game?" and
// TRC_XBOX_OPT_01 "Yes"
constexpr uint32_t kSaveSystem = 0x77F944;
constexpr uint32_t kSystemMessages = 0xA4;
constexpr uint32_t kOverwriteMessage = 0x34;
constexpr uint32_t kYesMessage = 0x18;
constexpr uint32_t kSystemChoices = 0x10;  // interface, vtable +8 chooses an option by index
constexpr uint32_t kChooseOption = 0x08;
constexpr size_t kMaxMessage = 256;

constexpr const char* kAutosavingText = "Autosaving...";

struct Box {
    uint32_t screen = 0;
    bool answered = false;
    bool shown = false;
};
Box g_box;

// the stats screen's own save: its box is opened at once, the close that would leave the screen is held back
enum class StatsSave { Idle, Saving, Saved, Left };
struct Stats {
    uint32_t screen = 0;
    uint32_t script = 0;
    StatsSave state = StatsSave::Idle;
};
Stats g_stats;

std::optional<uint32_t> g_overwrite_yes;  // the save system asked to overwrite, answered in the next tick

bool TakesInput(uint32_t screen) {
    bool active = R32(screen + kScreenActive) != 0;
    bool blocked = R32(screen + kScreenBlocked) || R32(screen + kScreenListener + kListenerHeld);
    return active && !blocked;
}

bool IsNamed(uint32_t clip, const char* name) { return std::strcmp(Name(clip), name) == 0; }

bool HasNamed(uint32_t clip, const char* name, int depth) {
    for (uint32_t child : Children(clip)) {
        if (IsNamed(child, name)) return true;
        if (depth > 1 && HasNamed(child, name, depth - 1)) return true;
    }
    return false;
}

// by name: a box's screen path can resolve to another clip of the same name than the one drawn
bool InSaveBox(uint32_t clip) {
    uint32_t at = clip;
    for (int i = 0; at && i < kMaxParents; i++, at = R32(at + kClipParent)) {
        if (Type(at) && IsNamed(at, kSaveBoxClip)) return true;
    }
    return false;
}

bool IsStatsPage(uint32_t clip) { return HasNamed(clip, kSaveBoxClip, 1) && HasNamed(clip, kStatsTitle, kTitleDepth); }

void CallOnActivate(X86Context& ctx, uint32_t screen, uint32_t index) {
    uint32_t script = R32(screen + kScreenScript);
    ctx.ecx = script;
    CallGuest(ctx, R32(R32(script) + kScriptOnActivate), {index});
}

// the box

// the box takes input only on its question; a second time means its save failed
void UpdateBox(X86Context& ctx, uint32_t screen, bool question) {
    if (screen != g_box.screen) g_box = {screen};
    if (g_box.shown) return;
    if (question) {
        g_box.shown = true;
        return;
    }
    if (!TakesInput(screen)) return;
    if (g_box.answered) {
        g_box.shown = true;
        return;
    }
    g_box.answered = true;
    CallOnActivate(ctx, screen, kYesRow);
}

// the stats screen takes input once its rewards are shown: its accept opens the box
void UpdateStats(X86Context& ctx, uint32_t screen) {
    if (screen != g_stats.screen) g_stats = {screen};
    if (g_stats.state != StatsSave::Idle || !TakesInput(screen)) return;
    g_stats.script = R32(screen + kScreenScript);
    g_stats.state = StatsSave::Saving;
    CallOnActivate(ctx, screen, 0);
}

// the overwrite question

std::u16string GuestWide(uint32_t text) {
    std::u16string out;
    for (uint32_t at = text; Readable(at) && out.size() < kMaxMessage; at += 2) {
        char16_t c = G<char16_t>(at);
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

std::u16string SystemMessage(uint32_t offset) {
    uint32_t system = R32(kSaveSystem);
    uint32_t messages = system ? R32(system + kSystemMessages) : 0;
    uint32_t text = messages ? R32(messages + offset) : 0;
    return text ? GuestWide(text) : std::u16string();
}

bool IsOverwriteQuestion(uint32_t question) {
    std::u16string overwrite = SystemMessage(kOverwriteMessage);
    return question && !overwrite.empty() && GuestWide(question) == overwrite;
}

std::optional<uint32_t> YesOption(uint32_t count, uint32_t options) {
    std::u16string yes = SystemMessage(kYesMessage);
    for (uint32_t i = 0; i < count && !yes.empty(); i++) {
        if (GuestWide(R32(options + i * 4)) == yes) return i;
    }
    return std::nullopt;
}

void ApplyOverwriteAnswer(X86Context& ctx) {
    if (!g_overwrite_yes) return;
    uint32_t yes = *g_overwrite_yes;
    g_overwrite_yes.reset();
    uint32_t choices = R32(kSaveSystem) + kSystemChoices;
    ctx.ecx = choices;
    CallGuest(ctx, R32(R32(choices) + kChooseOption), {yes});
}

bool Hiding() { return game::Opt().autosave && !g_box.shown; }

}  // namespace

// in the movie's Tick, where a pad's accept would run the page's script
void AutosaveFocus(X86Context& ctx) {
    if (!game::Opt().autosave) {
        g_box = {};
        g_stats = {};
        return;
    }
    ApplyOverwriteAnswer(ctx);
    uint32_t box = 0;
    bool question = false;
    uint32_t stats = 0;
    for (const MenuPorts& menu : ActiveMenuPorts()) {
        if (IsNamed(menu.clip, kSaveBoxClip)) box = menu.screen;
        if (IsNamed(menu.clip, kChoiceClip) && InSaveBox(menu.clip)) question = question || TakesInput(menu.screen);
        if (IsStatsPage(menu.clip)) stats = menu.screen;
    }
    if (box) UpdateBox(ctx, box, question);
    if (!box) g_box = {};
    if (stats) UpdateStats(ctx, stats);
    if (!stats) g_stats = {};
}

// a child clip draw: nothing in a save box is drawn
bool AutosaveHidesClip(uint32_t clip) { return Hiding() && InSaveBox(clip); }

// end of the frame render, over the finished frame
void DrawAutosave(X86Context& ctx, uint8_t*) {
    if (Hiding() && g_box.screen) DrawCornerText(ctx, MenuText(kAutosavingText), Corner::TopRight);
}

// The save system asks the savegame object a question, cdecl (question, option count, options), all wide text.
// The overwrite question of a box autosave answered never reaches it.
X86_EXTERN_FUNC(__imp__sub_0001CD3D);
X86_FUNC(sub_0001CD3D) {
    uint32_t question = R32(ctx.esp + 4);
    uint32_t count = R32(ctx.esp + 8);
    uint32_t options = R32(ctx.esp + 12);
    bool ours = g_box.answered && !g_box.shown && IsOverwriteQuestion(question);
    std::optional<uint32_t> yes = ours ? YesOption(count, options) : std::nullopt;
    if (yes) {
        g_overwrite_yes = yes;
        Return(ctx, 0, 0);
        return;
    }
    __imp__sub_0001CD3D(ctx, base);
}

// the page script's OnCloseChild, ecx the handler: the stats screen stays after its own save, taking input again
X86_EXTERN_FUNC(__imp__sub_00014D2C);
X86_FUNC(sub_00014D2C) {
    bool held = ctx.ecx == g_stats.script && g_stats.state == StatsSave::Saving;
    if (!held) {
        __imp__sub_00014D2C(ctx, base);
        return;
    }
    g_stats.state = StatsSave::Saved;
    W32(g_stats.screen + kScreenBlocked, 0);
    Return(ctx, 0, 0);
}

// the page script's OnActivate, thiscall (index): the saved stats screen leaves as its box's close would make it
X86_EXTERN_FUNC(__imp__sub_0001F0B0);
X86_FUNC(sub_0001F0B0) {
    bool saved = ctx.ecx == g_stats.script && g_stats.state == StatsSave::Saved;
    if (!saved) {
        __imp__sub_0001F0B0(ctx, base);
        return;
    }
    g_stats.state = StatsSave::Left;
    W32(g_stats.screen + kScreenBlocked, 1);
    CallGuest(ctx, kOnCloseChild, {});
    Return(ctx, 0, 1);
}
