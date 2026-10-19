#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "xdk/hle.h"

// the options menu's stock rows when it has this screen, else 0: its rows show in their places, scrolling
int OptionsMenuStockRows(uint32_t screen);
int OptionsMenuIndexAt(int slot);  // the screen index of the row in a stock row's place
void OpenOptionsPage(int page);    // the next pause options menu opens on the page, none for -1
void OptionsMenuFocus(X86Context& ctx);  // in the pad's turn
void ReselectRow(X86Context& ctx, uint32_t screen);  // the focused row selected again, for its move sound
// the recomp's text in the language the game shows, one Latin 1 byte a character as the game's own text is
std::string MenuText(std::string_view english, bool caps = true);
