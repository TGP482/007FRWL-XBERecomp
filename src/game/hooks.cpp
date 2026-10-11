#include "kernel/exports.h"
#include "xdk/hle.h"

// XGetFilePhysicalSortKey(handle): the FATX/GDFX volume query behind it has no host version.
// 0 gives every file the same disc position, which only changes the order the game queues loads.
X86_FUNC(sub_003196D2) {
    kernel::Return(ctx, 0, 1);
}

// cdecl, reached from the Direct3D library's debug messages (0x5F30A0)
KERNEL_EXPORT(8, DbgPrint, 0) {
    LOGI("DbgPrint: %s", static_cast<const char*>(kernel::HostPtr(a[0])));
    return 0;
}
