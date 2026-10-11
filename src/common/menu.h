#pragma once

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
void ToggleMenu();
void DrawUi(double fps);  // fps is unused until the counter exists

}  // namespace app
