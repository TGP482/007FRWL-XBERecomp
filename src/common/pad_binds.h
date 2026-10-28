#pragma once
#include <span>
#include <string>

#include "common/mouse_keyboard.h"

// the controller's buttons rebound: each action is one of the console pad's controls, pressed in play by the button
// bound to it; the menus keep the stock buttons
namespace input {

std::span<const Action> PadActions();
std::string PadBinding(size_t action);    // the button's name, empty for none
void BindPad(size_t action, int button);  // the action that had the button takes this one's old one, foot and car apart
void ResetPadBindings();
void LoadPadBindings();                   // after the settings load
void RemapPad(int port, Pad& pad);        // each poll: the held buttons pressed as the actions bound to them
void SetPadDriving(int port, bool driving);  // game thread, each input update: the port's car binds apply
int PressedPadButton();                   // a button held on any controller now, -1 for none

}  // namespace input
