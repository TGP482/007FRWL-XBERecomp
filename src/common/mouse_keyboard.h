#pragma once
#include <optional>
#include <span>
#include <string>

#include "core/input.h"

// the keyboard and mouse as the controller on port 0
namespace input {

void StartMouse(void* hwnd);    // window thread, the game window is up
void OnRawInput(void* handle);  // window thread, WM_INPUT
void ReleaseCursor();           // the game lost the input: the clip goes, held keys wait for release
void Keyboard(Pad& pad);        // held keys onto port 0's pad

// raw mouse counts since the last call, scaled by the settings; none when there is no motion
struct MouseMove {
    float dx;
    float dy;
};
std::optional<MouseMove> TakeMouse();  // game thread, each tick in play: asking is what marks play

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
