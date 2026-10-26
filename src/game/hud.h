#pragma once
#include <string_view>

#include "xdk/hle.h"

// draws until the matching false keep their shape at the screen's left: text the host adds to the HUD
void HudAnchorLeft(bool on);
void HudAnchorRight(bool on);  // the same at the screen's right; either false ends both
void HudAnchorCentre(bool on);  // the same in the middle 4:3
bool InFrontEnd();               // a front end menu's film is shown
bool FilmShown();                // a full screen film drew lately, such as the demo over the front end

// text over the finished frame in the game's font, called from a hook at the frame's end (fps_counter.cpp);
// drop moves it down by that share of the screen height
enum class Corner { TopLeft, TopRight, BottomRight };
void DrawCornerText(X86Context& ctx, std::string_view line, Corner corner, float drop = 0);
