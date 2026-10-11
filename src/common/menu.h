#pragma once
#include <cstddef>
#include <string>

// the F10 settings panel, drawn by DrawUi (GameInfo::ui)
namespace app {

bool MenuOpen();
bool Capturing();  // a key binding waits for a key
void ToggleMenu();
void DrawUi(double fps);  // fps is unused until the counter exists
// the game menus' cursor (HCURSOR): cursor.png beside the exe sized to the frame, else the system arrow
void* CursorImage();

// the panel's rows, for the game's own options menu
namespace rows {

enum class Kind { Heading, Value, Bind };
struct Row {
    Kind kind;
    int id;  // a bind row's input action
    const char* label;
};
const Row* Page(int page, size_t* count);         // 0 the settings, 1 mouse and keyboard
std::string Value(const Row& row, int step = 0);  // what the row shows, stepped first when step is not zero
void Defaults(int page);

}  // namespace rows

}  // namespace app
