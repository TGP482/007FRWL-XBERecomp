#pragma once
#include <array>
#include <atomic>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages the recomp text has: English, French, German, Italian, Spanish
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp"};

// what a player's controller port has plugged in: the keyboard and mouse, a controller, both or neither
struct PlayerPort {
    bool keyboard = false;
    int pad = -1;  // controller slot 0 to 3, -1 for none
};
inline constexpr int kPlayers = 4;  // the console's controller ports

struct Options {
    bool skip_intros = false;
    bool skippable_cutscenes = true;  // Start or its key ends an in game cutscene
    bool fps_counter = false;
    bool hdr_output = true;  // HDR10 on a display with Windows HDR on
    bool invert_mouse = false;
    bool camera_centring = false;  // the chase camera swings back behind Bond on keyboard and mouse
    bool free_aim = false;  // holding aim with any gun gives the rocket launcher's over the shoulder aim
    bool persistent_bodies = false;  // dead enemies and their dropped guns and shields are never deleted
    uint32_t ambient_occlusion = 0;  // screen space, from the scene's depth: 0 off, 1 SSAO, 2 GTAO
    uint32_t audio_output = 0;  // audio::kOutputAuto, kOutputStereo, kOutputSurround or kOutputMono
    float mouse_sensitivity = 1.0f;
    uint32_t msaa = 0;  // samples a pixel of the scene, 0 off
    bool smaa = false;  // over the finished frame, HUD included; combines with msaa
    uint32_t fov = 65;  // horizontal degrees on a 4:3 frame, 65 is the game's
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    uint32_t walk_speed = 50;  // percent of a full stick the walk key moves at
    std::string monitor;  // its device name, \\.\DISPLAY2; empty the primary
    bool pause_on_focus_loss = true;  // the game and its sound stop while the window is in the background
    bool async_shaders = true;  // a draw whose pipeline needs compiling is skipped until the build threads have it
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
    std::map<std::string, std::string> pad_buttons;  // action id to a controller button's name, absent is the default
    std::array<PlayerPort, kPlayers> players = {{{true, 0}, {false, 1}, {false, 2}, {false, 3}}};  // by port
};
Options& Opt();
const char* ShownLanguage();  // language.cpp
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
inline std::atomic<bool> g_hdr_display{false};  // the window's display has Windows HDR on, from the renderer
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the HUD is not drawn
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits for the window's focus
void WaitWhileUnfocused();  // focus_pause.cpp: game thread, before the present
uint32_t FocusPausedMs();  // the milliseconds the game thread has waited in focus pauses so far
bool OnGameThread();  // the calling thread is the one that waits in focus pauses
bool ControllerPlugged(int slot);  // game_info.cpp

}  // namespace game
