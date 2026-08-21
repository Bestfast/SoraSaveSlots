// SoraSaveSlots - registry of supported games. Adding a game: implement its
// resolve() in a new game_x.cpp and add it to kGames in dllmain.cpp.
#pragma once
#include "core.h"

extern const GameModule game_sora1;   // Trails in the Sky 1st Chapter
extern const GameModule game_sora2;   // Trails in the Sky 2nd Chapter (demo)
