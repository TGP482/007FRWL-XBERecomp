#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

// The inventory (the item select HUD) pauses the game while it is open, as on the console; with INVENTORY PAUSE
// off the game runs on behind it. The game switches its tick lists to a new mode only at a frame's start, so a
// pause undone in the same frame never takes effect.

using namespace kernel;

namespace {

constexpr uint32_t kGameMode = 0x826E20;
constexpr uint32_t kModePaused = 1;
constexpr uint32_t kModeRunning = 2;

}  // namespace


// TEMPORARY costume trace: the inventory's Bond preview steps through states 2 fade out, 3 wait for the new
// costume, 4 fade in, 1 idle
namespace {

constexpr uint32_t kCostumeState = 0x786FF4;
constexpr uint32_t kCostumeTimer = 0x787000;
constexpr uint32_t kPreviewLink = 0x787044;  // preview actor + 0x20
constexpr uint32_t kOldCostumeKey = 0x726BA8;
constexpr uint32_t kClockMs = 0x82435C;
constexpr uint32_t kFindPlayer = 0x21135;
constexpr uint32_t kPlayerModel = 0x4C;
constexpr uint32_t kPlayerModelData = 0x54;
constexpr uint32_t kActorAlpha = 0x150;
constexpr uint32_t kActorHidden = 0x15C;  // byte, hide count of vtable +0x6C and +0x70
constexpr uint32_t kActorWorldEntry = 0x68;
constexpr uint32_t kActorAttached = 0x6C;  // the world entry while vtable +0xF8 has it in the world
constexpr uint32_t kClumpParts = 0x40;     // model object: parts it draws

uint32_t g_player = 0;

uint32_t g_last_state = 0;
uint32_t g_last_log_ms = 0;

uint32_t FindPlayer(X86Context& ctx) {
    X86Context saved = ctx;
    uint32_t player = CallGuest(ctx, kFindPlayer, {});
    ctx = saved;
    return player;
}

// the costume key 0x363E60 reads from the player's model
uint32_t CostumeKey(uint32_t model) {
    uint32_t key = R32(model + 4);
    return key ? key : R32(model + 0xC) + 0x10;
}

void LogCostume(X86Context& ctx, const char* where) {
    uint32_t player = FindPlayer(ctx);
    g_player = player;
    uint32_t model = player ? R32(player + kPlayerModel) : 0;
    uint32_t key = model ? CostumeKey(model) : 0;
    uint32_t link = R32(kPreviewLink);
    float alpha = link ? G<float>(link - 0x20 + kActorAlpha) : -1.0f;
    if (player) {
        LOGR("costume %s: player hidden %u alpha %.2f entry %08X attached %08X parts %u", where,
             G<uint8_t>(player + kActorHidden), G<float>(player + kActorAlpha), R32(player + kActorWorldEntry),
             R32(player + kActorAttached), model ? R32(model + kClumpParts) : 0);
    }
    uint32_t preview_model = link ? R32(link - 0x20 + kPlayerModel) : 0;
    uint32_t preview_key = preview_model ? CostumeKey(preview_model) : 0;
    LOGR("costume %s: preview model %08X parts %u key %08X %08X %08X %08X player key %08X %08X %08X %08X", where,
         preview_model, preview_model ? R32(preview_model + kClumpParts) : 0, preview_key ? R32(preview_key) : 0,
         preview_key ? R32(preview_key + 4) : 0, preview_key ? R32(preview_key + 8) : 0,
         preview_key ? R32(preview_key + 12) : 0, key ? R32(key) : 0, key ? R32(key + 4) : 0, key ? R32(key + 8) : 0,
         key ? R32(key + 12) : 0);
    LOGR("costume %s: state %u mode %u clock %u timer %u player %08X model %08X data %08X key %08X %08X old %08X preview %08X alpha %.2f",
         where, R32(kCostumeState), R32(kGameMode), R32(kClockMs), R32(kCostumeTimer), player,
         model, player ? R32(player + kPlayerModelData) : 0, key ? R32(key) : 0, key ? R32(key + 4) : 0,
         R32(kOldCostumeKey), link, alpha);
}

}  // namespace

// EquipItem from the inventory script
X86_EXTERN_FUNC(__imp__sub_000204F6);
X86_FUNC(sub_000204F6) {
    LogCostume(ctx, "equip in");
    __imp__sub_000204F6(ctx, base);
    LogCostume(ctx, "equip out");
}

// inventory tick
X86_EXTERN_FUNC(__imp__sub_00019494);
X86_FUNC(sub_00019494) {
    __imp__sub_00019494(ctx, base);
    uint32_t state = R32(kCostumeState);
    uint32_t now = R32(kClockMs);
    bool waiting = state != 1 && now - g_last_log_ms >= 500;
    if (state == g_last_state && !waiting) return;
    g_last_state = state;
    g_last_log_ms = now;
    LogCostume(ctx, "tick");
}

// actor hide and show, thiscall: the player's, with the caller
X86_EXTERN_FUNC(__imp__sub_003254C0);
X86_FUNC(sub_003254C0) {
    uint32_t actor = ctx.ecx;
    uint32_t caller = R32(ctx.esp);
    __imp__sub_003254C0(ctx, base);
    if (actor != g_player || !actor) return;
    LOGR("costume: player hidden from %08X, count %u", caller, G<uint8_t>(actor + kActorHidden));
}

X86_EXTERN_FUNC(__imp__sub_003254F0);
X86_FUNC(sub_003254F0) {
    uint32_t actor = ctx.ecx;
    uint32_t caller = R32(ctx.esp);
    __imp__sub_003254F0(ctx, base);
    if (actor != g_player || !actor) return;
    LOGR("costume: player shown from %08X, count %u", caller, G<uint8_t>(actor + kActorHidden));
}

// actor fade, thiscall (ms, delete when done, flag)
X86_EXTERN_FUNC(__imp__sub_003255A0);
X86_FUNC(sub_003255A0) {
    uint32_t actor = ctx.ecx;
    uint32_t caller = R32(ctx.esp);
    uint32_t ms = R32(ctx.esp + 4);
    __imp__sub_003255A0(ctx, base);
    if (actor != g_player || !actor) return;
    LOGR("costume: player fade %u ms from %08X, alpha %.2f", ms, caller, G<float>(actor + kActorAlpha));
}

// the player's model rebuilt, refreshed, put in and taken out of the world, thiscall
#define COSTUME_TRACE(addr, what)                                                                       \
    X86_EXTERN_FUNC(__imp__sub_##addr);                                                                 \
    X86_FUNC(sub_##addr) {                                                                              \
        uint32_t actor = ctx.ecx;                                                                       \
        uint32_t caller = R32(ctx.esp);                                                                 \
        uint32_t arg = R32(ctx.esp + 4);                                                                \
        __imp__sub_##addr(ctx, base);                                                                   \
        uint32_t link = R32(kPreviewLink);                                                              \
        bool preview = link && actor == link - 0x20;                                                     \
        if (!preview && (actor != g_player || !actor)) return;                                          \
        LOGR("costume: %s %s(%08X) from %08X, eax %08X model %08X entry %08X attached %08X", preview ? "preview" : "player", what, arg, caller, \
             ctx.eax, R32(actor + kPlayerModel), R32(actor + kActorWorldEntry), R32(actor + kActorAttached)); \
    }

COSTUME_TRACE(00326260, "model set")
COSTUME_TRACE(003266B0, "model swap")
COSTUME_TRACE(00326610, "model refresh")
COSTUME_TRACE(00327230, "attach")
COSTUME_TRACE(003272C0, "detach")

// the inventory opens, thiscall on the HUD: item_select_hud goes on the port's controls and 0x3463D0 pauses
X86_EXTERN_FUNC(__imp__sub_00017BE4);
X86_FUNC(sub_00017BE4) {
    uint32_t mode = R32(kGameMode);
    __imp__sub_00017BE4(ctx, base);
    bool paused_here = mode == kModeRunning && R32(kGameMode) == kModePaused;
    if (paused_here && !game::Opt().inventory_pause) W32(kGameMode, kModeRunning);
    LogCostume(ctx, "open");
}
