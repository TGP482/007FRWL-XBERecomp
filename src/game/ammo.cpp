#include <cstdint>

#include "common/mouse_keyboard.h"
#include "xdk/hle.h"

// The switch ammo key swaps the gun in hand between its regular and special ammo as the inventory's ammo choice
// does (SetCurrentAmmo 0x1B9170), then reloads it so the new rounds are in the gun. That swap loads the clip at once
// with no animation and keeps each ammo's clip when it is swapped out, so both go back into store first.

using namespace kernel;

namespace {

constexpr uint32_t kGameMode = 0x826E20;
constexpr uint32_t kModeRunning = 2;
constexpr uint32_t kCutscenePlaying = 0x82E0BE;  // byte

// the players, one a port; each owner's vtable +8 gives the object that answers for the player
constexpr uint32_t kPlayers = 0x778E5C;
constexpr uint32_t kPlayersList = 0x04;
constexpr uint32_t kPlayersCount = 0x0C;
constexpr uint32_t kOwnerObject = 0x08;
constexpr uint32_t kPlayerVtable = 0x6A3D00;
constexpr uint32_t kMultiplayerVtable = 0x6B31A8;  // the multiplayer player built on it, 0x1D4D20
constexpr uint32_t kCurrentGun = 0x20311;  // thiscall on the player, the item in hand or 0
constexpr uint32_t kPlayerFlags = 0x5E4;
constexpr uint32_t kFlagReloading = 0x10;  // set while the reload state runs, cleared when it ends

// interface queries, thiscall vtable +0x14 (name, object out) with the names' pointers
constexpr uint32_t kQuerySlot = 0x14;
constexpr uint32_t kPlayerName = 0x7266E8;  // "Player"
constexpr uint32_t kGunName = 0x7261EC;     // "Gun"

// the gun's ammo kinds
constexpr uint32_t kGunAmmo = 0x300;
constexpr uint32_t kGunRegularAmmo = 0x304;
constexpr uint32_t kGunSpecialAmmo = 0x31C;
constexpr uint32_t kGunClip = 0x30C;
constexpr uint32_t kGunRounds = 0x1130C;   // thiscall (ammo): its rounds in the gun and in store
constexpr uint32_t kGunSetAmmo = 0x14110;  // thiscall (ammo, rounds to load), as the inventory calls it

// the gun's ammo table, records sorted by ammo: its rounds in store, then the clip kept from when it was swapped out
constexpr uint32_t kGunAmmoTable = 0x2F8;
constexpr uint32_t kTableRecords = 0x00;
constexpr uint32_t kTableCount = 0x04;
constexpr uint32_t kRecordBytes = 0x1C;
constexpr uint32_t kRecordStore = 0x04;
constexpr uint32_t kRecordKeptClip = 0x0C;

constexpr uint32_t kReloadAction = 0x83EEAB39;

// input updates left: the swap's own, then the one whose reload query is answered
constexpr int kReloadUpdates = 2;
int g_reload_wait = 0;
uint32_t g_reload_port = 0;

uint32_t Query(X86Context& ctx, uint32_t object, uint32_t name) {
    static const uint32_t out = mem::AllocSystem(4);
    if (!object) return 0;
    W32(out, 0);
    ctx.ecx = object;
    bool answered = (CallGuest(ctx, R32(R32(object) + kQuerySlot), {R32(name), out}) & 0xFF) != 0;
    return answered ? R32(out) : 0;
}

uint32_t PortPlayer(X86Context& ctx, int port) {
    uint32_t players = R32(kPlayers);
    if (!players || port < 0 || uint32_t(port) >= R32(players + kPlayersCount)) return 0;
    uint32_t owner = R32(R32(players + kPlayersList) + uint32_t(port) * 4);
    if (!owner) return 0;
    ctx.ecx = owner;
    uint32_t object = CallGuest(ctx, R32(R32(owner) + kOwnerObject), {});
    uint32_t player = Query(ctx, object, kPlayerName);
    uint32_t vtable = player ? R32(player) : 0;
    return vtable == kPlayerVtable || vtable == kMultiplayerVtable ? player : 0;
}

uint32_t HeldGun(X86Context& ctx, uint32_t player) {
    ctx.ecx = player;
    uint32_t item = CallGuest(ctx, kCurrentGun, {});
    return Query(ctx, item, kGunName);
}

uint32_t AmmoRecord(uint32_t gun, uint32_t ammo) {
    uint32_t table = R32(gun + kGunAmmoTable);
    if (!table) return 0;
    uint32_t records = R32(table + kTableRecords);
    uint32_t count = R32(table + kTableCount);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t record = records + i * kRecordBytes;
        if (R32(record) == ammo) return record;
    }
    return 0;
}

// the clip the swap would load back goes into store
void UnloadKeptClip(uint32_t gun, uint32_t ammo) {
    uint32_t record = AmmoRecord(gun, ammo);
    if (!record) return;
    W32(record + kRecordStore, R32(record + kRecordStore) + R32(record + kRecordKeptClip));
    W32(record + kRecordKeptClip, 0);
}

// the swap reloads at once with no animation; its clip goes back into store for the reload that follows
void UnloadClip(uint32_t gun) {
    uint32_t record = AmmoRecord(gun, R32(gun + kGunAmmo));
    if (!record) return;
    W32(record + kRecordStore, R32(record + kRecordStore) + R32(gun + kGunClip));
    W32(gun + kGunClip, 0);
}

// to the other ammo when the gun has both and rounds of it
bool SwapAmmo(X86Context& ctx, uint32_t gun) {
    uint32_t regular = R32(gun + kGunRegularAmmo);
    uint32_t special = R32(gun + kGunSpecialAmmo);
    if (!regular || !special) return false;
    uint32_t next = R32(gun + kGunAmmo) == special ? regular : special;
    ctx.ecx = gun;
    if (CallGuest(ctx, kGunRounds, {next}) == 0) return false;
    UnloadKeptClip(gun, next);
    ctx.ecx = gun;
    if (!(CallGuest(ctx, kGunSetAmmo, {next, 0}) & 0xFF)) return false;
    UnloadClip(gun);
    return true;
}

bool Reloading(uint32_t player) { return (R32(player + kPlayerFlags) & kFlagReloading) != 0; }

bool InPlay() { return R32(kGameMode) == kModeRunning && !G<uint8_t>(kCutscenePlaying); }

}  // namespace

// each input update, with the switch ammo key pressed in this one
void UpdateAmmoSwap(X86Context& ctx, bool pressed) {
    if (g_reload_wait > 0) g_reload_wait--;
    if (!pressed || !InPlay() || g_reload_wait > 0) return;
    X86Context saved = ctx;
    int port = input::KeyboardPort();
    uint32_t player = PortPlayer(ctx, port);
    uint32_t gun = player && !Reloading(player) ? HeldGun(ctx, player) : 0;
    bool swapped = gun && SwapAmmo(ctx, gun);
    ctx = saved;
    if (!swapped) return;
    g_reload_wait = kReloadUpdates;
    g_reload_port = uint32_t(port);
}

// the update after a swap answers the reload query of that port
bool ReloadForced(uint32_t port, uint32_t hash) {
    return g_reload_wait == 1 && hash == kReloadAction && port == g_reload_port;
}
