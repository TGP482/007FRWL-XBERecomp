#include <array>

#include "xdk/hle.h"

using namespace kernel;

// London's second area: the helicopter that fires at Bond and the top of the fountain (FuncSpawn
// "spawn_london _heli") lives in Zone04, but the stream set loading that area (stream_1_2_3) drops Zone04 just before
// the spawn, so the helicopter never appears. That set keeps Zone04 loaded here, as on GameCube.

namespace {

using ZoneId = std::array<uint32_t, 4>;

// stream set: ten zone ids from +0x64, an unused one all zero
constexpr uint32_t kZones = 0x64;
constexpr uint32_t kZoneSize = 0x10;

constexpr ZoneId kZone01 = {0xCA8DDB16, 0x453498CF, 0x25BD3FA8, 0x9E0DCDBB};
constexpr ZoneId kZone04 = {0x1D60DB1D, 0x4606608E, 0xABB1C59A, 0xFA5858F9};
constexpr ZoneId kZone05 = {0x14B0AF01, 0x489D8CAE, 0xEBB63BB7, 0x423AA8B0};
constexpr ZoneId kZone06 = {0x1C2FD4C5, 0x49E575D9, 0xB98F9DB9, 0x8F48B10D};
constexpr ZoneId kZone07 = {0xD0D9F9ED, 0x45691913, 0xB2BDDB97, 0xBE5CB00A};

constexpr std::array kSecondArea = {kZone01, kZone05, kZone06, kZone07};

uint32_t Slot(uint32_t set, size_t index) { return set + kZones + uint32_t(index) * kZoneSize; }

ZoneId ReadZone(uint32_t at) { return {R32(at), R32(at + 4), R32(at + 8), R32(at + 12)}; }

void WriteZone(uint32_t at, const ZoneId& zone) {
    for (uint32_t i = 0; i < zone.size(); ++i) W32(at + i * 4, zone[i]);
}

bool IsSecondAreaSet(uint32_t set) {
    for (size_t i = 0; i < kSecondArea.size(); ++i) {
        if (ReadZone(Slot(set, i)) != kSecondArea[i]) return false;
    }
    return ReadZone(Slot(set, kSecondArea.size())) == ZoneId{};
}

}  // namespace

// stream set runs (0x34E00), fastcall on the set
X86_EXTERN_FUNC(__imp__sub_00014241);
X86_FUNC(sub_00014241) {
    uint32_t set = ctx.ecx;
    if (IsSecondAreaSet(set)) WriteZone(Slot(set, kSecondArea.size()), kZone04);
    __imp__sub_00014241(ctx, base);
}
