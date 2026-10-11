#pragma once

namespace game {

// before boot: drops save folders an interrupted XCreateSaveGame left behind, which the game reports as damaged profiles
void RemoveUnfinishedSaves();

}  // namespace game
