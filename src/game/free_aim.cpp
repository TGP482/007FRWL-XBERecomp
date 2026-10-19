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

uint32_t g_player = 0;  // in the throw state through free aim
uint32_t g_frame = 0;

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

// throw state fire check, thiscall on the state: the gun fires there as the rocket launcher does
X86_EXTERN_FUNC(__imp__sub_0001F3F3);
X86_FUNC(sub_0001F3F3) {
    uint32_t state = ctx.ecx;
    uint32_t player = R32(state + kStatePlayer);
    uint32_t gun = 0;
    if (InFreeAim(player)) {
        ctx.ecx = player;
        gun = CallGuest(ctx, kCurrentGun, {});
    }
    uint32_t kind = gun ? R32(gun + kGunKind) : 0;
    if (gun) W32(gun + kGunKind, kKindRocketLauncher);
    ctx.ecx = state;
    __imp__sub_0001F3F3(ctx, base);
    if (gun) W32(gun + kGunKind, kind);
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
