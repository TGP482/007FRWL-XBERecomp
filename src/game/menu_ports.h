#pragma once
#include <cstdint>
#include <vector>

// the menus on screen and the controller ports they take, so each text's prompts follow its player's device
struct MenuPorts {
    uint32_t clip;   // the page's clip
    uint32_t ports;  // bit per controller port
    uint32_t screen;
};
std::vector<MenuPorts> ActiveMenuPorts();  // menu_cursor.cpp: every menu screen on the listener stack, lowest first
void SetPromptField(uint32_t field);       // prompts.cpp: the Apt text field being drawn, 0 after it
