#pragma once
// The game's Flash player: its levels and the clips on screen
#include <string.h>

#include <cstdint>
#include <vector>

#include "xdk/hle.h"

namespace flash {

// player
constexpr uint32_t kPlayer = 0x812560;
constexpr uint32_t kPlayerLevels = 0x28;

// clip
constexpr uint32_t kClipFlags = 0x04;   // type from bit 25, bit 4 set while it lives
constexpr uint32_t kClipName = 0x08;    // string
constexpr uint32_t kClipMatrix = 0x0C;  // a b c d tx ty
constexpr uint32_t kClipParent = 0x44;
constexpr uint32_t kClipData = 0x48;
constexpr uint32_t kClipNext = 0x50;
constexpr uint32_t kClipDepth = 0x54;  // low 17 bits, signed
constexpr uint32_t kClipVisible = 0x58;
constexpr uint32_t kVisibleBit = 0x100000;
constexpr uint32_t kLiveBit = 0x10;
constexpr uint32_t kTypeShift = 25;
constexpr uint32_t kTypeSprite = 0x0D;
constexpr uint32_t kTypeEditText = 0x0F;
constexpr uint32_t kTypeMovie = 0x12;
constexpr uint32_t kStringText = 0x08;
constexpr uint32_t kNoData = 0xBAADF00D;

// sprite data
constexpr uint32_t kSpriteCharacter = 0x0C;
constexpr uint32_t kSpriteFrame = 0x18;
constexpr uint32_t kSpriteChildren = 0x24;  // list: its head's next is the first child

// edit text data: +0x20 the game's text field, +0x54 its text as the movie gives it, +0x74 that text in the language
constexpr uint32_t kEditTextField = 0x20;
constexpr uint32_t kFieldText = 0x54;
constexpr uint32_t kFieldShownText = 0x74;

// sprite character: frames of {item count, items}, each item a pointer to {type, ...}
constexpr uint32_t kCharacterFrameCount = 0x08;
constexpr uint32_t kCharacterFrames = 0x0C;
constexpr uint32_t kFrameSize = 8;
constexpr uint32_t kFrameItems = 0x04;

constexpr size_t kMaxSiblings = 512;

inline bool Readable(uint32_t va) { return va && va != kNoData && mem::IsCommitted(va); }

inline const char* Text(uint32_t va) { return static_cast<const char*>(kernel::HostPtr(va)); }

inline uint32_t Type(uint32_t clip) {
    uint32_t flags = kernel::R32(clip + kClipFlags);
    return flags & kLiveBit ? flags >> kTypeShift : 0;
}

inline int Depth(uint32_t clip) { return int32_t(kernel::R32(clip + kClipDepth) << 15) >> 15; }

inline const char* Name(uint32_t clip) { return Text(kernel::R32(clip + kClipName) + kStringText); }

inline bool Shown(uint32_t clip) { return kernel::R32(clip + kClipVisible) & kVisibleBit; }

// a sprite's or a movie's data, 0 for other clips
inline uint32_t TimelineData(uint32_t clip) {
    uint32_t type = Type(clip);
    uint32_t data = type == kTypeSprite || type == kTypeMovie ? kernel::R32(clip + kClipData) : 0;
    return Readable(data) ? data : 0;
}

inline std::vector<uint32_t> Children(uint32_t clip) {
    std::vector<uint32_t> children;
    uint32_t data = TimelineData(clip);
    uint32_t list = data ? kernel::R32(data + kSpriteChildren) : 0;
    uint32_t head = Readable(list) ? kernel::R32(list) : 0;
    uint32_t child = Readable(head) ? kernel::R32(head + kClipNext) : 0;
    for (; child && children.size() < kMaxSiblings; child = kernel::R32(child + kClipNext)) children.push_back(child);
    return children;
}

inline std::vector<uint32_t> Levels() {
    std::vector<uint32_t> levels;
    uint32_t player = kernel::R32(kPlayer);
    uint32_t list = player ? kernel::R32(player + kPlayerLevels) : 0;
    uint32_t clip = list ? kernel::R32(list) : 0;
    for (; clip && levels.size() < kMaxSiblings; clip = kernel::R32(clip + kClipNext)) levels.push_back(clip);
    return levels;
}

inline uint32_t Level(int number) {
    for (uint32_t clip : Levels()) {
        if (Depth(clip) == number) return clip;
    }
    return 0;
}

inline uint32_t Named(const std::vector<uint32_t>& clips, const char* name) {
    for (uint32_t clip : clips) {
        if (_stricmp(Name(clip), name) == 0) return clip;
    }
    return 0;
}

// a sprite's character, 0 for other clips
inline uint32_t Character(uint32_t clip) {
    uint32_t data = Type(clip) == kTypeSprite ? TimelineData(clip) : 0;
    uint32_t character = data ? kernel::R32(data + kSpriteCharacter) : 0;
    return Readable(character) ? character : 0;
}

// an edit text's game text field, 0 for other clips
inline uint32_t EditTextField(uint32_t clip) {
    uint32_t data = Type(clip) == kTypeEditText ? kernel::R32(clip + kClipData) : 0;
    uint32_t field = Readable(data) ? kernel::R32(data + kEditTextField) : 0;
    return Readable(field) ? field : 0;
}

}  // namespace flash
