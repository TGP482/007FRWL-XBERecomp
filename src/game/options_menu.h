#pragma once
#include <cstdint>

// the options menu's stock rows when it has this screen, else 0: its rows show in their places, scrolling
int OptionsMenuStockRows(uint32_t screen);
int OptionsMenuIndexAt(int slot);  // the screen index of the row in a stock row's place
