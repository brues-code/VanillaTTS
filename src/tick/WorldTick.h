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

#pragma once

// Shared per-frame tick subscription. The engine's world-subsystem update at
// `FUN_WORLD_TICK` is the canonical once-per-frame hook target — single
// caller, quiet region, no known collisions with other Octo DLLs.
//
// Subscribers receive a callback at the TAIL of each frame (engine's original
// runs first). Declare a file-scope `static const Tick::WorldTick::AutoSubscribe`
// in your module; the constructor chains it onto the list at static-init time,
// before DllMain runs. `InstallHook()` is called once from DllMain after
// MH_Initialize.

namespace Tick::WorldTick {

using Callback = void (*)();

struct AutoSubscribe {
    explicit AutoSubscribe(Callback cb);
    Callback cb;
    AutoSubscribe *next;
};

// Installs the MinHook detour on FUN_WORLD_TICK. Returns false on failure
// (caller — DllMain — should propagate). Safe to call once.
bool InstallHook();

} // namespace Tick::WorldTick
