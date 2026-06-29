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

#include "WorldTick.h"

#include "MinHook.h"
#include "Offsets.h"

#include <windows.h>

namespace Tick::WorldTick {

namespace {

AutoSubscribe *g_head = nullptr;

using WorldTick_t = void(__fastcall *)(int, int, int);
WorldTick_t g_original = nullptr;

void __fastcall WorldTick_h(int a, int b, int c) {
    g_original(a, b, c);
    for (auto *node = g_head; node != nullptr; node = node->next)
        node->cb();
}

} // namespace

AutoSubscribe::AutoSubscribe(Callback cb) : cb(cb), next(g_head) {
    g_head = this;
}

bool InstallHook() {
    auto *target = reinterpret_cast<LPVOID>(Offsets::FUN_WORLD_TICK);
    if (MH_CreateHook(target, reinterpret_cast<LPVOID>(&WorldTick_h),
                      reinterpret_cast<LPVOID *>(&g_original)) != MH_OK)
        return false;
    if (MH_EnableHook(target) != MH_OK)
        return false;
    return true;
}

} // namespace Tick::WorldTick
