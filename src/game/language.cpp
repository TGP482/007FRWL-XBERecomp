// The language the game shows: its boot screen, language.swf, picks a row of text\localetable.txt
#include <cstring>

#include "common/options.h"
#include "core/log.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// the text manager, made at 0x434000
constexpr uint32_t kLocalization = 0x77F718;
constexpr uint32_t kLocaleTable = 0x0C;  // the table 0x435150 loaded
constexpr uint32_t kLocale = 0x28;       // the row it shows, the script's GetLanguage

// locale table: row count, rows of 0x14 bytes whose first field is the row's code
constexpr uint32_t kTableRows = 0x04;
constexpr uint32_t kTableRowArray = 0x08;
constexpr uint32_t kRowBytes = 0x14;
constexpr uint32_t kRowCode = 0x00;

// a setting code and the code its row of the locale table starts with
struct Language {
    const char* setting;  // game::kLanguages
    const char* locale;
};
constexpr Language kLocales[] = {{"fra", "fr"}, {"deu", "de"}, {"ita", "it"}, {"esp", "es"}};

// the code of the row the game shows, empty before the text manager and its table are made
const char* ShownCode() {
    uint32_t manager = R32(kLocalization);
    if (!manager) return "";
    uint32_t table = R32(manager + kLocaleTable);
    uint32_t row = R32(manager + kLocale);
    if (!table || row >= R32(table + kTableRows)) return "";
    uint32_t code = R32(R32(table + kTableRowArray) + row * kRowBytes + kRowCode);
    return code ? static_cast<const char*>(HostPtr(code)) : "";
}

}  // namespace

namespace game {

// the game's language now as one of kLanguages; English before the text manager is made and for its other rows
const char* ShownLanguage() {
    const char* code = ShownCode();
    const char* shown = game::kLanguages[0];
    for (const Language& l : kLocales) {
        if (std::strcmp(code, l.locale) == 0) shown = l.setting;
    }
    static const char* reported = nullptr;
    if (shown != reported) LOGR("language: game shows '%s', panel uses %s", code, shown);
    reported = shown;
    return shown;
}

}  // namespace game
