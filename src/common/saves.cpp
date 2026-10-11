// save folders on the emulated hard disk, U: of the title is hdd\E\UDATA\<title id>
#include "common/saves.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace fs = std::filesystem;

namespace game {

namespace {

constexpr wchar_t kTitleSaves[] = L"E/UDATA/45410079";
constexpr char kSaveMeta[] = "SaveMeta.xbx";

fs::path SaveRoot() {
    int instance = app::Instance();
    std::wstring hdd = instance > 1 ? L"hdd" + std::to_wstring(instance) : L"hdd";
    return app::ExeDir() / hdd / kTitleSaves;
}

// XCreateSaveGame creates the folder and SaveMeta.xbx, then writes the save's name into it: an empty SaveMeta.xbx
// alone was cut off before its name, holds no game data and can never be opened again
bool IsUnfinished(const fs::path& folder) {
    std::error_code ec;
    int files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, ec)) {
        files++;
        if (files > 1 || entry.path().filename() != kSaveMeta) return false;
    }
    if (ec || files != 1) return false;
    uintmax_t size = fs::file_size(folder / kSaveMeta, ec);
    return !ec && size == 0;
}

}  // namespace

void RemoveUnfinishedSaves() {
    std::error_code ec;
    std::vector<fs::path> unfinished;
    for (const fs::directory_entry& entry : fs::directory_iterator(SaveRoot(), ec)) {
        if (entry.is_directory(ec) && IsUnfinished(entry.path())) unfinished.push_back(entry.path());
    }
    for (const fs::path& folder : unfinished) {
        fs::remove_all(folder, ec);
        LOGR("saves: removed unfinished save %s", folder.filename().string().c_str());
    }
}

}  // namespace game
