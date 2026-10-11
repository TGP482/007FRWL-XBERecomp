#pragma once

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);  // fps is unused until the counter exists

}  // namespace app
