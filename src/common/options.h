#pragma once
#include <atomic>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

struct Options {
    bool skip_intros = false;
    bool fps_counter = false;
    bool invert_mouse = false;
    bool camera_centring = false;  // the chase camera swings back behind Bond on keyboard and mouse
    uint32_t ambient_occlusion = 0;  // screen space, from the scene's depth: 0 off, 1 SSAO, 2 GTAO
    float mouse_sensitivity = 1.0f;
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter

}  // namespace game
