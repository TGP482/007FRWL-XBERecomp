#include <cstring>

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "xdk/hle.h"

// Free aim: holding aim with any gun goes into the rocket launcher's over the shoulder aim, the throw state
// (PlayerThrowItemSM), instead of locking on. The gun is passed off as the rocket launcher only for the checks that
// let it in and let it fire there, and its shots go at the aim point the state keeps under the crosshair.

using namespace kernel;

namespace {

constexpr uint32_t kFrameCount = 0x824368;
constexpr uint32_t kLinkOffset = 0x20;  // object links point 0x20 into the object

// the player
constexpr uint32_t kPlayerVtable = 0x6A3D00;
constexpr uint32_t kPlayerPort = 0xB24;
constexpr uint32_t kPlayerAimFlags = 0x5E8;
constexpr uint32_t kAimFlagAiming = 0x001;  // shots go at the aim point
constexpr uint32_t kCurrentGun = 0x20311;    // thiscall, the gun in hand or 0

// the gun
constexpr uint32_t kGunOwner = 0x1CC;
constexpr uint32_t kGunKind = 0x270;
constexpr uint32_t kKindRocketLauncher = 10;  // the kind the throw state's fire check 0x13CC80 takes
constexpr uint32_t kGunFlags = 0x2B0;
constexpr uint32_t kGunAimsAsThrown = 0x400;  // read by the throw state's check 0x13C9B0

// items answer interface queries, thiscall vtable +0x14 (name, object out) as the names' pointers
constexpr uint32_t kQuerySlot = 0x14;
constexpr uint32_t kGunName = 0x7261EC;  // "Gun"
constexpr uint32_t kLaserWatchName = 0x7261FC;  // "LaserWatch": a gun too, left to its own state

// the throw state's own pointer to the player
constexpr uint32_t kStatePlayer = 0x4C;

// the throw state's action dispatch 0x13E0D0, thiscall: the frame seconds and the action from esp
constexpr uint32_t kActionSeconds = 8;
constexpr uint32_t kActionId = 0x10;
constexpr uint32_t kActionAimItem = 0x15;  // the firing state's update: the item aims at the aim point, no turn
constexpr uint32_t kAnimationArgs = 5;
constexpr uint32_t kAimUpdate = 0x14052;   // thiscall on the state (seconds): turn, pitch, aim point, root motion

// the fire button as the gun fire check 0x138290 reads it
constexpr uint32_t kGunSingleShot = 0x160;     // gun vtable slot, thiscall: a press a shot, else fires while held
constexpr uint32_t kPlayerInput = 0xAC0;       // its vtable +4 gives the port actions are read on
constexpr uint32_t kInputManager = 0x77F5BC;
constexpr uint32_t kActionPressed = 0x34DEC0;  // thiscall on the manager (port, action)
constexpr uint32_t kFireAction = 0xFF68BD8B;

// the player's RenderWare frame: its getter object, the matrix getter and the update, both cdecl(frame)
constexpr uint32_t kPlayerFrameGetter = 0x28;  // thiscall slot 0 on the object here returns the frame
constexpr uint32_t kFrameGetMatrix = 0x262A10;
constexpr uint32_t kFrameUpdateObjects = 0x262970;
constexpr uint32_t kMatrixAxesBytes = 0x30;  // right, up and at, each 16 bytes, before the position

uint32_t g_player = 0;  // in the throw state through free aim
uint32_t g_frame = 0;
uint32_t g_firing_player = 0;  // in the firing state through free aim
uint32_t g_firing_frame = 0;
// the aim update runs inside the firing state, which plays its own animation and moves the player itself
bool g_aim_while_firing = false;

bool KeyboardPlayer(uint32_t player) {
    return player && R32(player) == kPlayerVtable && int(R32(player + kPlayerPort)) == input::KeyboardPort();
}

bool Answers(X86Context& ctx, uint32_t item, uint32_t name) {
    static const uint32_t out = mem::AllocSystem(4);
    W32(out, 0);
    ctx.ecx = item;
    bool answered = (CallGuest(ctx, R32(R32(item) + kQuerySlot), {R32(name), out}) & 0xFF) != 0;
    return answered && R32(out);
}

bool IsGun(X86Context& ctx, uint32_t item) {
    return Answers(ctx, item, kGunName) && !Answers(ctx, item, kLaserWatchName);
}

bool FreeAimGun(X86Context& ctx, uint32_t player, uint32_t gun) {
    if (!game::Opt().free_aim || !gun || !KeyboardPlayer(player)) return false;
    return !(R32(gun + kGunFlags) & kGunAimsAsThrown) && IsGun(ctx, gun);
}

bool InFreeAim(uint32_t player) { return player && player == g_player && R32(kFrameCount) - g_frame <= 1; }

uint32_t CurrentGun(X86Context& ctx, uint32_t player) {
    ctx.ecx = player;
    return CallGuest(ctx, kCurrentGun, {});
}

bool SingleShot(X86Context& ctx, uint32_t gun) {
    ctx.ecx = gun;
    return (CallGuest(ctx, R32(R32(gun) + kGunSingleShot), {}) & 0xFF) != 0;
}

bool FirePressed(X86Context& ctx, uint32_t player) {
    ctx.ecx = player + kPlayerInput;
    uint32_t port = CallGuest(ctx, R32(R32(player + kPlayerInput) + 4), {});
    ctx.ecx = R32(kInputManager);
    return (CallGuest(ctx, kActionPressed, {port, kFireAction}) & 0xFF) != 0;
}

}  // namespace

// throw state check, cdecl(player, item in hand): in, and staying in, while aim is held
X86_EXTERN_FUNC(__imp__sub_00015CA4);
X86_FUNC(sub_00015CA4) {
    uint32_t player = R32(ctx.esp + 4);
    uint32_t gun = R32(ctx.esp + 8);
    bool lend = FreeAimGun(ctx, player, gun);
    uint32_t flags = lend ? R32(gun + kGunFlags) : 0;
    if (lend) W32(gun + kGunFlags, flags | kGunAimsAsThrown);
    __imp__sub_00015CA4(ctx, base);
    if (!lend) return;
    W32(gun + kGunFlags, flags);
    if (!(ctx.eax & 0xFF)) return;
    g_player = player;
    g_frame = R32(kFrameCount);
}

// throw state fire check, thiscall on the state: the gun fires there as the rocket launcher does. The check reads
// fire as held, so a single shot gun also needs a fresh press, as its own fire check asks
X86_EXTERN_FUNC(__imp__sub_0001F3F3);
X86_FUNC(sub_0001F3F3) {
    uint32_t state = ctx.ecx;
    uint32_t player = R32(state + kStatePlayer);
    uint32_t gun = InFreeAim(player) ? CurrentGun(ctx, player) : 0;
    uint32_t kind = gun ? R32(gun + kGunKind) : 0;
    if (gun) W32(gun + kGunKind, kKindRocketLauncher);
    ctx.ecx = state;
    __imp__sub_0001F3F3(ctx, base);
    if (!gun) return;
    W32(gun + kGunKind, kind);
    uint32_t fires = ctx.eax;
    if ((fires & 0xFF) && SingleShot(ctx, gun) && !FirePressed(ctx, player)) fires = 0;
    ctx.eax = fires;
}

// throw state action dispatch: each shot puts a free aim gun through the firing state (useItem), whose update aims
// the item but never turns, so the view stood still while firing. The aim update runs there first
X86_EXTERN_FUNC(__imp__sub_000137C9);
X86_FUNC(sub_000137C9) {
    uint32_t state = ctx.ecx;
    uint32_t player = R32(state + kStatePlayer);
    bool aim_item = R32(ctx.esp + kActionId) == kActionAimItem;
    if (aim_item && FreeAimGun(ctx, player, CurrentGun(ctx, player))) {
        g_firing_player = player;
        g_firing_frame = R32(kFrameCount);
        g_aim_while_firing = true;
        ctx.ecx = state;
        CallGuest(ctx, kAimUpdate, {R32(ctx.esp + kActionSeconds)});
        g_aim_while_firing = false;
    }
    ctx.ecx = state;
    __imp__sub_000137C9(ctx, base);
}

// player root motion, thiscall on the player: the firing animation's root sets the player's facing each frame, which
// undid the mouse turn. While a free aim gun fires only its move is kept
X86_EXTERN_FUNC(__imp__sub_00015D6C);
X86_FUNC(sub_00015D6C) {
    uint32_t player = ctx.ecx;
    if (g_aim_while_firing) {
        Return(ctx, 0, 0);
        return;
    }
    bool firing = player == g_firing_player && R32(kFrameCount) - g_firing_frame <= 1;
    if (!firing) {
        __imp__sub_00015D6C(ctx, base);
        return;
    }
    ctx.ecx = player + kPlayerFrameGetter;
    uint32_t frame = CallGuest(ctx, R32(R32(player + kPlayerFrameGetter)), {});
    uint32_t matrix = CallGuest(ctx, kFrameGetMatrix, {frame});
    uint8_t axes[kMatrixAxesBytes];
    std::memcpy(axes, HostPtr(matrix), sizeof(axes));
    ctx.ecx = player;
    __imp__sub_00015D6C(ctx, base);
    std::memcpy(HostPtr(matrix), axes, sizeof(axes));
    CallGuest(ctx, kFrameUpdateObjects, {frame});
}

// state animation, thiscall on the state (animation, 4 more): the aim pose would replace the firing animation, whose
// end the firing state waits for
X86_EXTERN_FUNC(__imp__sub_0033CDF0);
X86_FUNC(sub_0033CDF0) {
    if (!g_aim_while_firing) {
        __imp__sub_0033CDF0(ctx, base);
        return;
    }
    Return(ctx, 0, kAnimationArgs);
}

// gun fire frame, thiscall(frame, normalise) on the gun: free aim shots go at the aim point
X86_EXTERN_FUNC(__imp__sub_00019BD3);
X86_FUNC(sub_00019BD3) {
    uint32_t link = R32(ctx.ecx + kGunOwner);
    uint32_t player = link ? link - kLinkOffset : 0;
    if (!InFreeAim(player)) {
        __imp__sub_00019BD3(ctx, base);
        return;
    }
    uint32_t flags = R32(player + kPlayerAimFlags);
    uint32_t added = ~flags & kAimFlagAiming;
    W32(player + kPlayerAimFlags, flags | added);
    __imp__sub_00019BD3(ctx, base);
    W32(player + kPlayerAimFlags, R32(player + kPlayerAimFlags) & ~added);
}
