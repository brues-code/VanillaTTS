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

#include "Custom.h"

#include "Offsets.h"

#include <cstdint>
#include <cstring>

namespace Event::Custom {

namespace {

// Reservations registered by `AutoReserve` at static-init time, before
// any engine code runs. `RetryClaims` (triggered by the
// `Frame::RegisterEvent` hook) walks this list and claims a NULL slot
// for each unclaimed entry.
constexpr int MAX_RESERVED = 32;
struct ReservedName {
    const char *name;
    int slot;  // -1 until claimed
};
ReservedName g_reserved[MAX_RESERVED];
int g_reservedCount = 0;
bool g_writesEnabled = false;

// Engine's SStrDup at `FUN_STORM_SSTRDUP` — uses `SMemAlloc` internally
// and is exactly what the engine itself does when filling event table
// entries. Reload teardown's `SMemFree(entry.name)` validates that the
// pointer came from `SMemAlloc`, so handing it an SStrDup'd block is
// the only safe way to inject a name.
char *SStrDup(const char *s) {
    if (s == nullptr)
        return nullptr;
    using SStrDup_t = char *(__stdcall *)(const char *src, const char *file,
                                          int line);
    auto fn = reinterpret_cast<SStrDup_t>(Offsets::FUN_STORM_SSTRDUP);
    return fn(s, __FILE__, __LINE__);
}

// Walk the engine's event table from the END looking for a NULL-name
// slot. The bulk-init leaves ~200 NULL gaps scattered through the
// input-names array (it only writes ~350 of the slots explicitly; the
// rest stay zero-initialized). We go DOWN so our events group at the
// tail, visually distinct from engine events.
int TryClaim(const char *eventName) {
    if (!g_writesEnabled)
        return -1;
    auto *base = *reinterpret_cast<uint8_t **>(Offsets::VAR_EVENT_TABLE_BASE_PTR);
    const int count = *reinterpret_cast<int *>(Offsets::VAR_EVENT_TABLE_COUNT);
    if (base == nullptr || count <= 0)
        return -1;
    for (int i = count - 1; i >= 0; --i) {
        auto **namePtr = reinterpret_cast<const char **>(
            base + i * Offsets::EVENT_ENTRY_STRIDE +
            Offsets::OFF_EVENT_ENTRY_NAME);
        if (*namePtr == nullptr) {
            char *copy = SStrDup(eventName);
            if (copy == nullptr)
                return -1;
            *namePtr = copy;
            return i;
        }
    }
    return -1;
}

} // namespace

AutoReserve::AutoReserve(const char *name) {
    if (name == nullptr || g_reservedCount >= MAX_RESERVED)
        return;
    for (int i = 0; i < g_reservedCount; ++i) {
        if (std::strcmp(g_reserved[i].name, name) == 0)
            return;
    }
    g_reserved[g_reservedCount].name = name;
    g_reserved[g_reservedCount].slot = -1;
    ++g_reservedCount;
}

int Lookup(const char *name) {
    if (name == nullptr)
        return -1;
    for (int i = 0; i < g_reservedCount; ++i) {
        if (std::strcmp(g_reserved[i].name, name) == 0)
            return g_reserved[i].slot;
    }
    return -1;
}

int LookupByName(const char *name) {
    if (name == nullptr)
        return -1;
    auto *base = *reinterpret_cast<uint8_t **>(Offsets::VAR_EVENT_TABLE_BASE_PTR);
    const int count = *reinterpret_cast<int *>(Offsets::VAR_EVENT_TABLE_COUNT);
    if (base == nullptr || count <= 0)
        return -1;
    for (int i = 0; i < count; ++i) {
        const char *entryName = *reinterpret_cast<const char *const *>(
            base + i * Offsets::EVENT_ENTRY_STRIDE +
            Offsets::OFF_EVENT_ENTRY_NAME);
        if (entryName != nullptr && std::strcmp(entryName, name) == 0)
            return i;
    }
    return -1;
}

void RetryClaims() {
    for (int i = 0; i < g_reservedCount; ++i) {
        if (g_reserved[i].slot < 0)
            g_reserved[i].slot = TryClaim(g_reserved[i].name);
    }
}

void EnableWrites() { g_writesEnabled = true; }

void PrepareForReload() {
    for (int i = 0; i < g_reservedCount; ++i)
        g_reserved[i].slot = -1;
    g_writesEnabled = false;
}

} // namespace Event::Custom
