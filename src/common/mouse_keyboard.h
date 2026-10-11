#pragma once
#include <span>
#include <string>

#include "core/input.h"

// the keyboard as the controller on port 0
namespace input {

void Settle();            // the game lost the input: held keys wait for release before they press again
void Keyboard(Pad& pad);  // held keys onto port 0's pad

// actions the keyboard can be bound to, key names comma separated
struct Action {
    const char* id;
    const char* label;
    const char* defaults;
};
std::span<const Action> Actions();
std::string Binding(size_t action);
void Bind(size_t action, int vk);
void ResetBindings();
void LoadBindings();    // after the settings load
int PollCapturedKey();  // a bindable key or mouse button held now, 0 for none; marks a capture as polling
bool PanelKey(int vk);  // the settings panel is bound to this key and no key capture wants it
bool CaptureRecent();   // a key capture polled a moment ago

}  // namespace input
