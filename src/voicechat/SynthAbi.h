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

#include "voicechat/Backend.h"

#include <cstdint>

// ABI shared between the core DLL (VanillaTTS.dll) and the software-synth DLL
// (VanillaTTS_synth.dll). The core lazy-LoadLibrary's the synth DLL and calls
// its exported factory; the returned ITtsBackend reports playback lifecycle
// back through the host callbacks. Both DLLs are built from the same source
// with the same compiler/settings, so passing a C++ ITtsBackend* across the
// boundary is safe; the factory itself is plain C linkage for GetProcAddress.

namespace VoiceChat::Tts {

// Callbacks the synth backend invokes (on the MAIN thread, from its Pump())
// to drive the core's Lua event surface without linking the core. utteranceID
// correlates to the value the core passed into ITtsBackend::Speak().
struct SynthHost {
    void (*started)(uint32_t utteranceID);
    void (*finished)(uint32_t utteranceID);
    void (*failed)(uint32_t utteranceID, const char *status);
};

// Signature of the synth DLL's exported factory (resolved via GetProcAddress
// under the name kCreateBackendExport). Returns a backend owned by the synth
// DLL — valid until the DLL is unloaded — or null if the engine couldn't
// initialize. `host` must outlive the backend.
using CreateBackendFn = ITtsBackend *(__cdecl *)(const SynthHost *host);

// The exported factory symbol name.
constexpr const char *kCreateBackendExport = "VanillaTTS_CreateBackend";

} // namespace VoiceChat::Tts
