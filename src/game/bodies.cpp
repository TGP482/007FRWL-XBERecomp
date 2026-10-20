#include <deque>
#include <unordered_set>

#include "common/options.h"
#include "xdk/hle.h"

using namespace kernel;

// Persistent bodies: the newest dead enemies, and the guns and shields they drop, are never faded out and deleted.
// Older ones fade as on the console. A kept body animates only while its animation is still loaded: the game frees
// it a while after the console would have deleted the body.

namespace {

constexpr size_t kKeptBodies = 32;
constexpr size_t kKeptItems = 32;

constexpr uint32_t kGameMs = 0x824360;

// actor fade, thiscall (ms, delete when done, flag 8)
constexpr uint32_t kFade = 0x3255A0;
constexpr uint32_t kFadeMs = 1000;

// actor animation update 0x33B720, thiscall (seconds, forced)
constexpr uint32_t kAnimateArgs = 2;

// animation instance at actor +0x190: the set it plays from, the playing animation's data and id
constexpr uint32_t kActorAnimation = 0x190;
constexpr uint32_t kAnimationSet = 0x04;
constexpr uint32_t kAnimationData = 0x0C;
constexpr uint32_t kAnimationId = 0x10;
constexpr uint32_t kFindAnimation = 0x3EE300;  // cdecl (set, id), the loaded data or 0

// enemy: its two held items, each a pointer 0x20 into the item
constexpr uint32_t kHeldItems = 0xAF0;
constexpr uint32_t kHeldItemStep = 8;
constexpr uint32_t kHeldItemOffset = 0x20;
constexpr int kHeldItemCount = 2;

// dropped item: its life, and the time it starts to fade
constexpr uint32_t kItemLife = 0x230;
constexpr uint32_t kItemFadeAt = 0x234;
// an unset fade time lets the item leave the physics world once it rests (0xA71E0), which a riot shield's
// destructor 0xA7FA0 does not expect: it read the freed body at level unload
constexpr uint32_t kNeverFade = 0xFFFFFFFF;

// an actor as it was when kept; a deleted one is forgotten by its destructor
struct Kept {
    uint32_t actor = 0;
    uint32_t vtable = 0;
};

struct Body {
    Kept body;
    Kept held[kHeldItemCount];
};

std::deque<Body> g_bodies;
std::deque<Kept> g_items;
std::unordered_set<uint32_t> g_posed;  // kept bodies, faded ones too until deleted

Kept Keep(uint32_t actor) { return actor ? Kept{actor, R32(actor)} : Kept{}; }

bool Alive(const Kept& kept) {
    return kept.actor && mem::IsCommitted(kept.actor) && R32(kept.actor) == kept.vtable;
}

void Fade(X86Context& ctx, const Kept& kept, bool remove) {
    if (!Alive(kept)) return;
    X86Context saved = ctx;
    ctx.ecx = kept.actor;
    CallGuest(ctx, kFade, {kFadeMs, remove ? 1u : 0u, 0});
    ctx = saved;
}

// the fades the console gives a dead enemy: its held items, then the body, whose fade deletes it
void FadeBody(X86Context& ctx, const Body& body) {
    for (const Kept& held : body.held) Fade(ctx, held, false);
    Fade(ctx, body.body, true);
}

void FadeItem(const Kept& item) {
    if (Alive(item)) W32(item.actor + kItemFadeAt, R32(kGameMs) + R32(item.actor + kItemLife));
}

Body KeptBody(uint32_t enemy) {
    Body body{Keep(enemy)};
    for (int i = 0; i < kHeldItemCount; i++) {
        uint32_t held = R32(enemy + kHeldItems + uint32_t(i) * kHeldItemStep);
        if (held && held != kHeldItemOffset) body.held[i] = Keep(held - kHeldItemOffset);
    }
    return body;
}

void Forget(Kept& kept, uint32_t actor) {
    if (kept.actor == actor) kept = {};
}

}  // namespace

// enemy death done (0x46D70, every enemy kind), ebx the enemy: after the corpse leaves the collision, true skips the
// second long fade of the body and its held items, the last of which deletes the body
bool KeepCorpse(X86Context& ctx, uint8_t*) {
    if (!game::Opt().persistent_bodies) return false;
    g_bodies.push_back(KeptBody(ctx.ebx));
    g_posed.insert(ctx.ebx);
    if (g_bodies.size() > kKeptBodies) {
        Body oldest = g_bodies.front();
        g_bodies.pop_front();
        FadeBody(ctx, oldest);
    }
    return true;
}

// item dropped (0xA6CE0), esi the item: true keeps its fade timer from ever running out, instead of a minute then a
// two second fade and delete
bool KeepDroppedItem(X86Context& ctx, uint8_t*) {
    if (!game::Opt().persistent_bodies) return false;
    W32(ctx.esi + kItemFadeAt, kNeverFade);
    g_items.push_back(Keep(ctx.esi));
    if (g_items.size() > kKeptItems) {
        FadeItem(g_items.front());
        g_items.pop_front();
    }
    return true;
}

// actor destructor, thiscall: a deleted body or item leaves the kept lists
X86_EXTERN_FUNC(__imp__sub_0033C220);
X86_FUNC(sub_0033C220) {
    uint32_t actor = ctx.ecx;
    for (Body& body : g_bodies) {
        Forget(body.body, actor);
        for (Kept& held : body.held) Forget(held, actor);
    }
    for (Kept& item : g_items) Forget(item, actor);
    g_posed.erase(actor);
    __imp__sub_0033C220(ctx, base);
}

// the playing animation is still the one its set holds, not freed data
static bool AnimationLoaded(X86Context& ctx, uint32_t actor) {
    uint32_t instance = R32(actor + kActorAnimation);
    if (!instance) return false;
    X86Context saved = ctx;
    uint32_t data = CallGuest(ctx, kFindAnimation, {R32(instance + kAnimationSet), R32(instance + kAnimationId)});
    ctx = saved;
    return data && data == R32(instance + kAnimationData);
}

// actor animation update: a kept body whose animation is gone skips it and holds its pose
X86_EXTERN_FUNC(__imp__sub_0033B720);
X86_FUNC(sub_0033B720) {
    if (g_posed.count(ctx.ecx) && !AnimationLoaded(ctx, ctx.ecx)) {
        Return(ctx, 0, kAnimateArgs);
        return;
    }
    __imp__sub_0033B720(ctx, base);
}
