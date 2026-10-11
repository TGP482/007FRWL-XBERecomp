#pragma once
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

struct Options {
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();

}  // namespace game
