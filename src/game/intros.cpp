#include <cstring>

#include "common/options.h"
#include "xdk/hle.h"

// Skip intros: the logo films and the legal screen of the boot flow never start.
// The flow (flow/flow.flo) runs language, initialload, mgm, eag, legal, title, then the start screen.

namespace {

// flow node handlers, thiscall with the phase, the node and an extra, ret 0xC
constexpr uint32_t kPhaseBegin = 0;
constexpr uint32_t kPhaseArg = 4;
constexpr uint32_t kNodeArg = 8;
constexpr uint32_t kHandlerArgs = 3;

// flow node: type, name pointer, flags
constexpr uint32_t kNodeName = 4;

// the boot logos end where the start screen begins, later title films belong to the story
bool g_boot = true;

const char* NodeName(X86Context& ctx) {
    uint32_t node = kernel::R32(ctx.esp + kNodeArg);
    return static_cast<const char*>(kernel::HostPtr(kernel::R32(node + kNodeName)));
}

bool Begins(X86Context& ctx) { return kernel::R32(ctx.esp + kPhaseArg) == kPhaseBegin; }

bool StartsWith(const char* text, const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; }

bool IsBootLogo(const char* name) {
    return StartsWith(name, "mgm") || StartsWith(name, "eag_") || StartsWith(name, "title_");
}

}  // namespace

// movie node: ending at once lets the flow go on to the next node
X86_EXTERN_FUNC(__imp__sub_00018369);
X86_FUNC(sub_00018369) {
    bool skip = g_boot && game::Opt().skip_intros && Begins(ctx) && IsBootLogo(NodeName(ctx));
    if (skip) {
        kernel::Return(ctx, 0, kHandlerArgs);
        return;
    }
    __imp__sub_00018369(ctx, base);
}

// screen node: the legal screen is skipped the same way, the start screen ends the boot
X86_EXTERN_FUNC(__imp__sub_00014147);
X86_FUNC(sub_00014147) {
    if (Begins(ctx)) {
        const char* name = NodeName(ctx);
        if (std::strcmp(name, "start") == 0) g_boot = false;
        if (g_boot && game::Opt().skip_intros && std::strcmp(name, "legal") == 0) {
            kernel::Return(ctx, 0, kHandlerArgs);
            return;
        }
    }
    __imp__sub_00014147(ctx, base);
}
