#pragma once
#include <cstdint>

#include "xdk/hle.h"

// the options menu's stock rows when it has this screen, else 0: its rows show in their places, scrolling
int OptionsMenuStockRows(uint32_t screen);
int OptionsMenuIndexAt(int slot);  // the screen index of the row in a stock row's place
void OpenOptionsPage(int page);    // the next pause options menu opens on the page, none for -1
void OptionsMenuFocus(X86Context& ctx);  // in the pad's turn
