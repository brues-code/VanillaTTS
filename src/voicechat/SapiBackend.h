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

#include <mutex>
#include <unordered_map>

// Forward-declared so the header doesn't pull <sapi.h> into every TU that
// only needs to construct or reference the backend.
struct ISpVoice;

namespace VoiceChat::Tts {

// Windows SAPI 5 backend. Created on the main (game) thread under an STA;
// SAPI's SetNotifyCallbackFunction marshals stream events back to the
// creating thread's message pump (WoW pumps messages every frame), so the
// notify handler runs on the main thread and may drive the Lua event surface
// via Playback::.
class SapiBackend : public ITtsBackend {
public:
    bool Init() override;
    std::vector<VoiceEntry> Voices() override;
    bool Speak(int voiceID, const std::wstring &text, int rate, int volume,
               uint32_t utteranceID) override;
    void Stop() override;

private:
    void InitCom();
    bool SelectVoice(int voiceID);
    void OnNotify(); // drains SAPI stream events on the main thread

    // SAPI's SPNOTIFYCALLBACK is a context-free `void __stdcall(WPARAM,
    // LPARAM)`; we register `this` as the WPARAM so the thunk can recover
    // the instance.
    static void __stdcall NotifyThunk(uintptr_t self, long /*lParam*/);

    ISpVoice *m_voice = nullptr;
    bool m_comInit = false;
    std::mutex m_streamMx;
    std::unordered_map<unsigned long, uint32_t> m_streamToUtterance;
};

} // namespace VoiceChat::Tts
