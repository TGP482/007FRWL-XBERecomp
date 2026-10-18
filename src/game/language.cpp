// The language the game shows: its boot screen, language.swf, picks a row of text\localetable.txt
#include <cstdlib>
#include <string>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the text manager, made at 0x434000
constexpr uint32_t kLocalization = 0x77F718;
constexpr uint32_t kLocale = 0x28;  // the row of the locale table it shows, the script's GetLanguage
constexpr const char* kTablePath = "D:\\text\\localetable.txt";

// a setting code and the code its row of the locale table starts with
struct Language {
    const char* setting;  // game::kLanguages
    const char* locale;
};
constexpr Language kLocales[] = {{"fra", "fr"}, {"deu", "de"}, {"ita", "it"}, {"esp", "es"}};

// the rows after the version line as the loader 0x435150 reads them: four fields, five from version 1
std::vector<std::string> ParseCodes(const std::vector<uint8_t>& file) {
    std::vector<std::string> fields;
    std::string line;
    for (uint8_t c : file) {
        if (c != '\n' && c != '\r') {
            line += char(c);
            continue;
        }
        if (!line.empty()) fields.push_back(line);
        line.clear();
    }
    if (!line.empty()) fields.push_back(line);
    std::vector<std::string> codes;
    if (fields.empty()) return codes;
    size_t columns = std::atoi(fields[0].c_str()) >= 1 ? 5 : 4;
    for (size_t at = 1; at + columns <= fields.size(); at += columns) codes.push_back(fields[at]);
    return codes;
}

// the first field of each row of the game's locale table, by row
const std::vector<std::string>& LocaleCodes() {
    static const std::vector<std::string> codes = [] {
        std::vector<uint8_t> file;
        if (!ReadGameFile(kTablePath, file)) LOGW("language: no %s", kTablePath);
        return ParseCodes(file);
    }();
    return codes;
}

}  // namespace

namespace game {

// the game's language now as one of kLanguages; English before the text manager is made and for its other rows
const char* ShownLanguage() {
    const char* english = game::kLanguages[0];
    uint32_t manager = R32(kLocalization);
    if (!manager) return english;
    uint32_t row = R32(manager + kLocale);
    const std::vector<std::string>& codes = LocaleCodes();
    if (row >= codes.size()) return english;
    for (const Language& l : kLocales) {
        if (codes[row] == l.locale) return l.setting;
    }
    return english;
}

}  // namespace game
