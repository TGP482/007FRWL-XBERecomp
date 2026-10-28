#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/input.h"

// the keyboard and mouse as the controller of the player port they are set to
namespace input {

void StartMouse(void* hwnd);    // window thread, the game window is up
void OnRawInput(void* handle);  // window thread, WM_INPUT
void ReleaseCursor();           // the game lost the input: the clip goes, held keys wait for release
void Keyboard(Pad& pad);        // held keys onto the keyboard port's pad
int KeyboardPort();             // the player port the keyboard and mouse are on, -1 for none

// raw mouse counts since the last call, scaled by the settings; none when there is no motion
struct MouseMove {
    float dx;
    float dy;
};
std::optional<MouseMove> TakeMouse();  // game thread, each tick in play: asking is what marks play

// actions the keyboard can be bound to, key names comma separated
// letter, digit and symbol keys bind by position as the US key there; Binding and ButtonKey show the player's labels
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
    const char* section = nullptr;  // the heading its bind sits under, none for the on foot keys
};
std::span<const Action> Actions();
std::string Binding(size_t action);
// slot 0 the key, 1 its alternate (the F10 panel's second box); vk 0 clears the slot. A key the other slot has moves.
void Bind(size_t action, int vk, int slot = 0);
// every key off the action; neither this nor Bind with vk 0 leaves the settings panel without a key
void Unbind(size_t action);
// Delete went down in the game window since the last call, the menus' key for clearing a binding
bool TakeDeletePress();
void ResetBindings();
void LoadBindings();    // after the settings load
int PollCapturedKey();  // a bindable key or mouse button held now, 0 for none; marks a capture as polling
bool PanelKey(int vk);  // the settings panel is bound to this key and no key capture wants it
bool HudKey(int vk);    // the toggle HUD action is bound to this key and no key capture wants it
bool TexturesKey(int vk);  // the same for the toggle texture replacement action
bool CaptureRecent();   // a key capture polled a moment ago

// the actions with a key down now, a bit per Actions() index; none while the game does not have the keys
uint64_t HeldActions();
int ActionIndex(std::string_view id);  // -1 for none

// the controls the keyboard player has now, from the control map on top of their port
enum class Section { Foot, Inventory, Car, Jetpack, Copter };
void SetSection(Section section);  // game thread, each input update

// the game's own menus
void SeenPad(int port, const Pad& pad);  // a controller's report before the keys join it: its input hides the cursor
void SetMenu(bool menu);       // game thread, each tick: a menu page takes the input
void SetMenuWheelAcross(bool across);  // the menu's items are a row: the wheel steps left and right
bool InMenu();                 // a menu page takes the input, whichever device is used
bool KeyboardActive();         // keyboard or mouse used last, not a controller
bool LeftButton();             // the left mouse button held in the game window
bool CursorShown();            // in a menu with the keyboard or mouse used last
bool InPlay();                 // the game runs, not paused or in a cutscene: it asked for the mouse a moment ago

// the cursor over the shown frame, 0 to 1 across and down, and the buttons held now
struct Pointer {
    float x;
    float y;
    bool left;
    bool right;
};
std::optional<Pointer> Cursor();  // none while it is not shown

// a short press on the keyboard port's pad: the cursor gives the menus their own input
enum class Tap { Start, Back, Up, Down, Left, Right, A, B };
void Press(Tap tap);

// a menu page's own accept and cancel: Enter gives the one, the pause keys the other
struct PageKeys {
    Tap accept;
    Tap cancel;
};
void SetPageKeys(std::optional<PageKeys> keys);  // game thread, each tick; none outside a menu page

// the first key bound to a pad button as the panel names it, empty for none: 0 to 7 A B X Y black white and the
// triggers, 8 to 15 dpad up down left right, start, back, the thumbs
std::string ButtonKey(int button);

}  // namespace input
