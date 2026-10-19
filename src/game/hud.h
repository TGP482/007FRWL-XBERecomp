#pragma once
#include <string_view>

#include "xdk/hle.h"

// draws until the matching false keep their shape at the screen's left: text the host adds to the HUD
void HudAnchorLeft(bool on);
void HudAnchorRight(bool on);  // the same at the screen's right; either false ends both

// text over the finished frame in the game's font, called from a hook at the frame's end (fps_counter.cpp)
enum class Corner { TopLeft, TopRight };
void DrawCornerText(X86Context& ctx, std::string_view line, Corner corner);
