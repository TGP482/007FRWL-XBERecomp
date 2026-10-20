// Skip pop ups. TIPS: the tutorial, attache case, schematic and key boxes that pause the game are never shown, a
// pickup still plays the box's sound. OBJECTIVES: the objective messages, their flare and sound are skipped; the
// messages still queue and time out unseen. ALL: both.
#include <algorithm>
#include <set>
#include <string>

#include "common/options.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// skip_popups bits
constexpr uint32_t kSkipPausing = 1;
constexpr uint32_t kSkipObjectives = 2;

bool Skips(uint32_t bit) { return (game::Opt().skip_popups & bit) != 0; }

constexpr int kShowArgs = 3;      // header, text, continue prompt; ret 0xC
constexpr int kCueArgs = 2;       // player, kind; ret 8
constexpr int kScoreCueArgs = 1;  // player; ret 4

// HUD message as the player's message queue sends it: shown flag, text, alpha
constexpr uint32_t kMessageShown = 0x00;
constexpr uint32_t kMessageText = 0x04;

std::set<std::string> g_objective_texts;
bool g_stat_cue = false;  // the cue comes from a stat counter's star, not an objective

constexpr uint32_t kHud = 0x77F4B4;
constexpr uint32_t kHudSound = 0x142BE;  // thiscall on the HUD (sound index)
constexpr uint32_t kPopupSound = 9;      // the one the box plays as it opens, 0xED5AB

// Header text ids of the boxes that only teach; every other box is a pickup (case, schematic, key, gadget, weapon,
// costume). Ids hash the string table key as 0x3A7E60 does.
constexpr const char* kTutorialHeaders[] = {
    "LOND_HEADER_01", "LOND_HEADER_03", "LOND_HEADER_04", "LOND_HEADER_07", "LOND_HEADER_10", "LOND_HEADER_12",
    "LOND_HEADER_15", "MAZE_HEADER_01", "MAZE_HEADER_02", "MAZE_HEADER_04", "MAZE_HEADER_05", "MAZE_HEADER_06",
    "CNSL_HEADER_03", "SNPR_HEADER_01", "STAT_HEADER_02", "UNDR_HEADER_02", "IST1_HEADER_01", "COMBAT_HEADER_01",
};

constexpr uint32_t TextId(const char* key) {
    uint32_t id = 0;
    for (; *key; key++) {
        uint32_t c = static_cast<uint8_t>(*key);
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        id = id * 0x1003F + c;
    }
    return id;
}
static_assert(TextId("TEXT_PRESSTOCONTINUE") == 0x57D63333);

bool IsTutorial(uint32_t header) {
    auto same = [header](const char* key) { return TextId(key) == header; };
    return std::any_of(std::begin(kTutorialHeaders), std::end(kTutorialHeaders), same);
}

void PlayPopupSound(X86Context& ctx) {
    X86Context saved = ctx;
    ctx.ecx = R32(kHud);
    CallGuest(ctx, kHudSound, {kPopupSound});
    ctx = saved;
}

std::string GuestText(uint32_t address) {
    if (!address) return {};
    return static_cast<const char*>(HostPtr(address));
}

}  // namespace

// HUD shows a tutorial box, thiscall: 0xDF550 moves to the box at HUD+0xA00, 0xED3F0 pauses until A.
// Every one comes from a level's TutorialText (0xED190), which still marks it seen.
X86_EXTERN_FUNC(__imp__sub_0001947B);
X86_FUNC(sub_0001947B) {
    if (Skips(kSkipPausing)) {
        uint32_t header = R32(ctx.esp + 4);
        if (!IsTutorial(header)) PlayPopupSound(ctx);
        Return(ctx, 0, kShowArgs);
        return;
    }
    __imp__sub_0001947B(ctx, base);
}

// an objective's message for the player, thiscall (text string, sound callback): only the objective classes call it
X86_EXTERN_FUNC(__imp__sub_00021D97);
X86_FUNC(sub_00021D97) {
    uint32_t text_string = R32(ctx.esp + 4);
    g_objective_texts.insert(GuestText(R32(text_string)));
    __imp__sub_00021D97(ctx, base);
}

// HUD message slot of a player, thiscall (message, player); the message is the caller's copy
X86_EXTERN_FUNC(__imp__sub_00021D65);
X86_FUNC(sub_00021D65) {
    uint32_t message = R32(ctx.esp + 4);
    bool shown = R32(message + kMessageShown) != 0;
    bool objective = g_objective_texts.count(GuestText(R32(message + kMessageText))) != 0;
    if (shown && objective && Skips(kSkipObjectives)) W32(message + kMessageShown, 0);
    __imp__sub_00021D65(ctx, base);
}

// The objective flare and sound: the HUD starts the effect in front of the camera and the sound together, for the
// message callback (0xE59A0, thiscall: player, kind) and the score objectives' complete and fail (0xE5A30, 0xE5AD0)
X86_EXTERN_FUNC(__imp__sub_0001802B);
X86_FUNC(sub_0001802B) {
    if (Skips(kSkipObjectives) && !g_stat_cue) {
        Return(ctx, 0, kCueArgs);
        return;
    }
    __imp__sub_0001802B(ctx, base);
}

// stat counter callback (focus shots, stealth kills, combo hits, 0x22C710): its star cue goes through 0xE59A0 too
X86_EXTERN_FUNC(__imp__sub_00017F81);
X86_FUNC(sub_00017F81) {
    g_stat_cue = true;
    __imp__sub_00017F81(ctx, base);
    g_stat_cue = false;
}

X86_EXTERN_FUNC(__imp__sub_00015FC4);
X86_FUNC(sub_00015FC4) {
    if (Skips(kSkipObjectives)) {
        Return(ctx, 0, kScoreCueArgs);
        return;
    }
    __imp__sub_00015FC4(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0001D0DA);
X86_FUNC(sub_0001D0DA) {
    if (Skips(kSkipObjectives)) {
        Return(ctx, 0, kScoreCueArgs);
        return;
    }
    __imp__sub_0001D0DA(ctx, base);
}
