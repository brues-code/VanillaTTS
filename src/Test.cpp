// This file is part of VanillaTTS.
//
// VanillaTTS is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// VanillaTTS is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// VanillaTTS. If not, see <https://www.gnu.org/licenses/>.

// Plumbing canary. Registers a single global so an in-game
//   /dump VanillaTTS_Ping()
// proves the LoadScriptFunctions hook fired, the module auto-register list
// ran, and a Lua C function we injected is callable — independently of the
// TTS feature. Remove once the core is verified in-world.

#include "Game.h"

namespace {

int __fastcall Script_Ping(void *L) {
    Game::Lua::PushString(L, "VanillaTTS alive");
    Game::Lua::PushNumber(L, 1);
    return 2;
}

void RegisterLuaFunctions() {
    Game::Lua::RegisterGlobalFunction("VanillaTTS_Ping", &Script_Ping);
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace
