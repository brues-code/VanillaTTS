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

#include "Offsets.h"

namespace Event::Custom {

// Reserve an event name to be claimed in the engine's event table at
// the next safe opportunity. Place a static instance at file scope:
//
//   static const Event::Custom::AutoReserve _r{"MY_EVENT"};
//
// Static-init chains the name onto an internal list *before* `DllMain`
// runs. After the engine and any other DLLs have finished writing to
// the event table (signaled by the first `Frame::RegisterEvent` call
// from Lua, which our hook intercepts to fire `RetryClaims`), we walk
// the table from the END looking for NULL-name slots and claim them
// for our reserved names — engine-owned `SStrDup` storage, so the
// engine's reload teardown frees them correctly.
//
// We don't hook `RebuildEventTable` directly: chaining with other DLLs
// that hook the same function (SuperWoWhook, nampower, transmogfix)
// led to count→buffer-size mismatches and crashes in the engine's fill
// loop. The slot-claim approach lets each DLL operate on the table
// independently of the others.
//
// The name pointer must outlive the engine (a string literal does).
// Same name reserved twice is deduped; reserving more than 32 names
// total silently drops the overflow.
struct AutoReserve {
    explicit AutoReserve(const char *name);
};

// Returns the slot id currently assigned to `name`, or -1 if not yet
// claimed (e.g. before the first Lua-side `RegisterEvent` has
// triggered `RetryClaims`). Slot indices may change across `/reload`,
// so call this at fire time rather than caching the value.
int Lookup(const char *name);

// Walks the live engine event table at `[VAR_EVENT_TABLE_BASE_PTR]`
// looking for an entry whose name strcmps equal to `name`. Returns
// the slot index, or -1 if no entry matches. Works for both
// engine-defined events AND our `AutoReserve`-claimed custom events.
int LookupByName(const char *name);

// Dispatches a custom event via the engine's printf-style event
// dispatcher at `FUN_FIRE_EVENT`. `format` is a concatenation of `%d`
// (int), `%u` (uint), `%f` (double), `%s` (const char *) tokens — one
// per payload arg, no separators or literal text. String args must
// outlive the call (engine doesn't copy them out of varargs);
// compile-time literals are fine. No-op for `eventID < 0`.
template <typename... Args>
inline void Fire(int eventID, const char *format, Args... args) {
    if (eventID < 0)
        return;
    using FireEventFn_t = void(__cdecl *)(int eventID, const char *format, ...);
    auto fn = reinterpret_cast<FireEventFn_t>(Offsets::FUN_FIRE_EVENT);
    fn(eventID, format, args...);
}

// Internal: try to claim a slot for every reservation that's still
// unclaimed (`slot < 0`). Called from the `Frame::RegisterEvent` hook.
void RetryClaims();

// Internal: permit `TryClaim` to write to the event table. Held closed
// until `LoadScriptFunctions_h` returns — writing during the engine's
// boot-time `RegisterEvent` flurry crashes in `SMemFree` on slots it
// still considers in-flight.
void EnableWrites();

// Internal: invalidate cached slot indices before `/reload`. The engine
// rebuilds the event table at a fresh allocation.
void PrepareForReload();

} // namespace Event::Custom
