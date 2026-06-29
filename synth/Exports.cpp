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

#include "SynthBackend.h"
#include "voicechat/SynthAbi.h"

namespace {
// Process-lifetime singleton. Deliberately leaked (never deleted) so the
// worker thread is never joined under the loader lock at unload — the OS
// reclaims it at process exit. The core treats the returned pointer as valid
// for the DLL's lifetime.
VoiceChat::Tts::SynthBackend *g_backend = nullptr;
} // namespace

// Factory resolved by the core via GetProcAddress(kCreateBackendExport).
extern "C" __declspec(dllexport) VoiceChat::Tts::ITtsBackend *__cdecl
VanillaTTS_CreateBackend(const VoiceChat::Tts::SynthHost *host) {
    if (host == nullptr)
        return nullptr;
    if (g_backend == nullptr)
        g_backend = new VoiceChat::Tts::SynthBackend(*host);
    return g_backend;
}
