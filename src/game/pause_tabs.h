#pragma once
#include <cstdint>

#include "xdk/hle.h"

// The recomp's pages as tabs below the pause menu's own, drawn as copies of them. The tab screen keeps its four
// items: on a recomp tab it stays on the last one, whose panel shows the options
constexpr int kStockTabs = 4;
constexpr int kExtraTabs = 3;

void UpdatePauseTabs(uint32_t screen);  // each frame: the pause menu's tab screen while it takes input, else 0
void PointPauseTab(int tab);            // the cursor is on a tab
void PauseTabsFocus(X86Context& ctx);   // in the pad's turn
bool TakePauseTabButton(X86Context& ctx, uint32_t screen, uint32_t code);
bool HoldPauseTabButton(uint32_t screen);  // a held direction does not repeat past the stock tabs
const char* PauseTabLabel();  // the text a field drawn now shows instead of its own, else null
bool DrawingPauseTab();       // a stock tab of the column draws now
