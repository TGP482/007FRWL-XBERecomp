#include <array>
#include <cstdint>
#include <cstring>
#include <optional>

#include "common/mouse_keyboard.h"
#include "common/pad_binds.h"
#include "xdk/hle.h"

// The game reads its controls as named actions: each is a pattern of the pad's buttons (a press, a hold of 100 ms)
// in the control map on top of the player port's stack. Many share a button, A alone is jump, climb, use and,
// held, reload. For the keyboard player the actions below are answered from their own keys instead, so a key does
// only what it is bound to.

using namespace kernel;

void UpdateAmmoSwap(X86Context& ctx, bool pressed);  // ammo.cpp
bool ReloadForced(uint32_t port, uint32_t hash);     // ammo.cpp

namespace {

using input::Section;

// an action by the hash of its name (0x3A7E60), the controls it belongs to and the keyboard actions that answer it
struct Answer {
    uint32_t hash;
    Section section;
    std::array<const char*, 2> keys;
};

constexpr Answer kAnswers[] = {
    // basic_player
    {0x7DDCA04E, Section::Foot, {"interact"}},   // jump
    {0x2A1213D2, Section::Foot, {"interact"}},   // vault
    {0x97DEE18E, Section::Foot, {"interact"}},   // step_up
    {0xF7E02080, Section::Foot, {"interact"}},   // vault_and_climb
    {0x196A4515, Section::Foot, {"interact"}},   // climb
    {0xC4CCF1F7, Section::Foot, {"interact"}},   // use_object
    {0xA2D67105, Section::Foot, {"interact"}},   // enter_vehicle
    {0x83EEAB39, Section::Foot, {"reload"}},     // reload
    {0xCBC9B2A3, Section::Foot, {"inventory"}},  // itemselect_activate
    {0x3AF81AAA, Section::Foot, {"target_left"}},   // switch_target_left, a flick of the right stick
    {0xEFD019D9, Section::Foot, {"target_right"}},  // switch_target_right
    // item_select_hud
    {0x9D72D5CB, Section::Inventory, {"inventory_up"}},      // itemselect_up
    {0x3453D1D2, Section::Inventory, {"inventory_down"}},    // itemselect_down
    {0xA38CB897, Section::Inventory, {"inventory_left"}},    // itemselect_left
    {0x4A4FF72C, Section::Inventory, {"inventory_right"}},   // itemselect_right
    {0x2E6B9A8C, Section::Inventory, {"inventory_select"}},  // itemselect_select
    {0x18078D8A, Section::Inventory, {"inventory_cancel"}},  // itemselect_cancel
    // AstonMartinPlayerDriver; the steering is the left stick's
    {0x72D75DDD, Section::Car, {"car_accelerate"}},    // accelerate
    {0xE8B44E0B, Section::Car, {"car_brake"}},         // brake
    {0x094BF238, Section::Car, {"car_steer_left"}},    // steerleft
    {0x8E0025CB, Section::Car, {"car_steer_right"}},   // steerright
    {0x24A64FDC, Section::Car, {"car_handbrake"}},     // handbrake
    {0x4E7F3FB2, Section::Car, {"car_fire"}},          // fireweapon
    // a press picks the next target, holding keeps the lock, both on LT as on the console
    {0x7FC16C7C, Section::Car, {"car_target_lock"}},   // targetlock
    {0xAC012CA4, Section::Car, {"car_target_lock"}},   // nexttarget
    {0x600284C2, Section::Car, {"car_cycle_weapon"}},  // cycleweapon
    {0x42D8C8B3, Section::Car, {"car_tire_slasher"}},  // usegadget
    {0x811C91EB, Section::Car, {"car_exit"}},          // exit_vehicle
    // the factory's vehicle drives under basic_player, on the right stick's gas and brake, or these actions
    {0x72D75DDD, Section::Foot, {"car_accelerate"}},  // accelerate
    {0xE8B44E0B, Section::Foot, {"car_brake"}},       // brake
    // jetpackplayer
    {0x2FC31762, Section::Jetpack, {"fly_up"}},    // thrust_up
    {0xEAEEB7E9, Section::Jetpack, {"fly_down"}},  // thrust_down
    {0xD1120BEB, Section::Jetpack, {"fly_exit"}},  // land
    // qcopterplayer
    {0x2FC31762, Section::Copter, {"fly_up"}},    // thrust_up
    {0xEAEEB7E9, Section::Copter, {"fly_down"}},  // thrust_down
    {0x65156A6F, Section::Copter, {"fly_exit"}},  // disengage
};

// the input manager: a record a player port, each with a stack of control maps whose top one is matched
constexpr uint32_t kInputManager = 0x77F5BC;
constexpr uint32_t kPortRecordBytes = 0x170;
constexpr uint32_t kMapDepth = 0x10;  // the maps pushed; map n at kMapDepth + n * 4
constexpr uint32_t kMaxMapDepth = 8;
constexpr uint32_t kMapName = 9;  // in the control map
constexpr size_t kMapNameBytes = 0x20;

struct MapSection {
    const char* name;
    Section section;
};
constexpr MapSection kMapSections[] = {
    {"item_select_hud", Section::Inventory},
    {"AstonMartinPlayerDriver", Section::Car},
    {"jetpackplayer", Section::Jetpack},
    {"qcopterplayer", Section::Copter},
};

// how the game asks: held now, or pressed since the last update
constexpr uint32_t kHeld = 0;
constexpr uint32_t kPressed = 1;
constexpr uint32_t kPortMask = 0xFF;
constexpr uint32_t kPorts = 4;

// basic_player's idle: the left stick at rest for 200 ms. Its map is hidden under item_select_hud, so with the
// inventory open and the game running idle never comes and Bond walks on; it is answered for the open inventory
constexpr uint32_t kIdleAction = 0x46F5C7B4;

// the keys as the input manager's last update saw them, a bit per keyboard action
struct KeyFrame {
    uint64_t now = 0;
    uint64_t before = 0;
};
KeyFrame g_keys;

uint64_t KeyMask(const Answer& answer) {
    uint64_t mask = 0;
    for (const char* id : answer.keys) {
        int index = id ? input::ActionIndex(id) : -1;
        if (index >= 0) mask |= uint64_t(1) << index;
    }
    return mask;
}

uint32_t TopMap(int port) {
    uint32_t manager = R32(kInputManager);
    if (!manager || port < 0) return 0;
    uint32_t record = manager + uint32_t(port) * kPortRecordBytes;
    uint32_t depth = R32(record + kMapDepth);
    return depth && depth <= kMaxMapDepth ? R32(record + kMapDepth + depth * 4) : 0;
}

Section SectionOf(int port) {
    uint32_t map = TopMap(port);
    if (!map) return Section::Foot;
    const char* name = static_cast<const char*>(HostPtr(map + kMapName));
    for (const MapSection& known : kMapSections) {
        if (std::strncmp(name, known.name, kMapNameBytes) == 0) return known.section;
    }
    return Section::Foot;
}

Section g_section = Section::Foot;

struct InventoryFrame {
    bool open = false;
    bool opened = false;  // this update
};
std::array<InventoryFrame, kPorts> g_inventory;

void LatchInventory() {
    for (uint32_t port = 0; port < kPorts; port++) {
        InventoryFrame& frame = g_inventory[port];
        bool open = SectionOf(int(port)) == Section::Inventory;
        frame.opened = open && !frame.open;
        frame.open = open;
    }
}

std::optional<bool> IdleAnswer(uint32_t port, uint32_t hash, uint32_t mode) {
    if (hash != kIdleAction || port >= kPorts || !g_inventory[port].open) return std::nullopt;
    if (mode == kHeld) return true;
    if (mode == kPressed) return g_inventory[port].opened;
    return std::nullopt;
}

const Answer* FindAnswer(uint32_t hash) {
    for (const Answer& answer : kAnswers) {
        if (answer.hash == hash && answer.section == g_section) return &answer;
    }
    return nullptr;
}

// none leaves the action to the pad
std::optional<bool> KeyAnswer(uint32_t port, uint32_t hash, uint32_t mode) {
    bool keyboard = int(port) == input::KeyboardPort() && input::KeyboardActive();
    const Answer* answer = keyboard && mode <= kPressed ? FindAnswer(hash) : nullptr;
    if (!answer) return std::nullopt;
    uint64_t keys = mode == kHeld ? g_keys.now : g_keys.now & ~g_keys.before;
    return (keys & KeyMask(*answer)) != 0;
}

bool KeyPressed(const char* id) {
    int index = input::ActionIndex(id);
    uint64_t pressed = g_keys.now & ~g_keys.before;
    return index >= 0 && (pressed >> index & 1);
}

}  // namespace

// the input manager's update, once a frame whether paused or running, before it matches the ports' actions
X86_EXTERN_FUNC(__imp__sub_0034FEB0);
X86_FUNC(sub_0034FEB0) {
    g_keys.before = g_keys.now;
    g_keys.now = input::HeldActions();
    g_section = SectionOf(input::KeyboardPort());
    LatchInventory();
    input::SetSection(g_section);
    for (int port = 0; port < int(kPorts); port++) input::SetPadDriving(port, SectionOf(port) == Section::Car);
    bool on_foot = input::KeyboardActive() && g_section == Section::Foot;
    UpdateAmmoSwap(ctx, on_foot && KeyPressed("switch_ammo"));
    __imp__sub_0034FEB0(ctx, base);
}

// the action query, thiscall on the input manager (port, hash, mode)
X86_EXTERN_FUNC(__imp__sub_0034DDE0);
X86_FUNC(sub_0034DDE0) {
    uint32_t port = R32(ctx.esp + 4) & kPortMask;
    uint32_t hash = R32(ctx.esp + 8);
    uint32_t mode = R32(ctx.esp + 12);
    std::optional<bool> keys = IdleAnswer(port, hash, mode);
    if (!keys && ReloadForced(port, hash)) keys = true;
    if (!keys) keys = KeyAnswer(port, hash, mode);
    if (!keys) return __imp__sub_0034DDE0(ctx, base);
    Return(ctx, *keys ? 1 : 0, 3);
}
