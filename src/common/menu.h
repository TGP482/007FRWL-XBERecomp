#pragma once

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);  // fps is unused until the counter exists
// the game menus' cursor (HCURSOR): cursor.png beside the exe sized to the frame, else the system arrow
void* CursorImage();

}  // namespace app
