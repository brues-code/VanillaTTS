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

#include "voicechat/SapiBackend.h"

#include <initguid.h> // instantiate the SAPI GUIDs in this TU (no sapi.lib)
#include <sapi.h>
#include <windows.h>

#include <algorithm>

namespace VoiceChat::Tts {

void SapiBackend::InitCom() {
    if (m_comInit)
        return;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // S_FALSE = already initialized on this thread; treat as success.
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        m_comInit = true;
}

void __stdcall SapiBackend::NotifyThunk(uintptr_t self, long) {
    auto *backend = reinterpret_cast<SapiBackend *>(self);
    if (backend != nullptr)
        backend->OnNotify();
}

void SapiBackend::OnNotify() {
    if (m_voice == nullptr)
        return;
    SPEVENT ev = {};
    ULONG fetched = 0;
    while (SUCCEEDED(m_voice->GetEvents(1, &ev, &fetched)) && fetched == 1) {
        if (ev.eEventId == SPEI_START_INPUT_STREAM) {
            uint32_t id = 0;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_streamMx);
                auto it = m_streamToUtterance.find(ev.ulStreamNum);
                if (it != m_streamToUtterance.end()) {
                    id = it->second;
                    found = true;
                }
            }
            if (found)
                Playback::Started(id);
        } else if (ev.eEventId == SPEI_END_INPUT_STREAM) {
            uint32_t id = 0;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_streamMx);
                auto it = m_streamToUtterance.find(ev.ulStreamNum);
                if (it != m_streamToUtterance.end()) {
                    id = it->second;
                    m_streamToUtterance.erase(it);
                    found = true;
                }
            }
            if (found)
                Playback::Finished(id);
        }
        // Only START/END stream events are requested; neither carries an
        // allocated lParam, so no SpClearEvent (sphelper) is needed.
        ev = SPEVENT{};
    }
}

bool SapiBackend::Init() {
    if (m_voice != nullptr)
        return true;
    InitCom();
    HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
                                  IID_ISpVoice, reinterpret_cast<void **>(&m_voice));
    if (FAILED(hr) || m_voice == nullptr) {
        m_voice = nullptr;
        return false;
    }
    m_voice->SetNotifyCallbackFunction(
        reinterpret_cast<SPNOTIFYCALLBACK *>(&NotifyThunk),
        reinterpret_cast<WPARAM>(this), 0);
    const ULONGLONG interest =
        SPFEI(SPEI_START_INPUT_STREAM) | SPFEI(SPEI_END_INPUT_STREAM);
    m_voice->SetInterest(interest, interest);

    // Availability gate: SAPI under Wine is frequently present but voiceless
    // (zero installed voices). Treat that as "unavailable" so the selector
    // falls through to the Flite software synth.
    if (Voices().empty()) {
        m_voice->Release();
        m_voice = nullptr;
        return false;
    }
    return true;
}

std::vector<VoiceEntry> SapiBackend::Voices() {
    InitCom();
    std::vector<VoiceEntry> voices;

    ISpObjectTokenCategory *cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void **>(&cat))) ||
        cat == nullptr)
        return voices;

    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
        IEnumSpObjectTokens *en = nullptr;
        if (SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en != nullptr) {
            ISpObjectToken *tok = nullptr;
            ULONG fetched = 0;
            int index = 0;
            while (en->Next(1, &tok, &fetched) == S_OK && fetched == 1) {
                LPWSTR desc = nullptr;
                if (SUCCEEDED(tok->GetStringValue(nullptr, &desc)) && desc != nullptr) {
                    voices.push_back({index, desc});
                    CoTaskMemFree(desc);
                }
                tok->Release();
                ++index;
            }
            en->Release();
        }
    }
    cat->Release();
    return voices;
}

// Select the voice at enumeration index `voiceID` on m_voice.
bool SapiBackend::SelectVoice(int voiceID) {
    if (m_voice == nullptr)
        return false;
    ISpObjectTokenCategory *cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                IID_ISpObjectTokenCategory,
                                reinterpret_cast<void **>(&cat))) ||
        cat == nullptr)
        return false;

    bool set = false;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE))) {
        IEnumSpObjectTokens *en = nullptr;
        if (SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en != nullptr) {
            ISpObjectToken *tok = nullptr;
            ULONG fetched = 0;
            int index = 0;
            while (en->Next(1, &tok, &fetched) == S_OK && fetched == 1) {
                if (index == voiceID) {
                    set = SUCCEEDED(m_voice->SetVoice(tok));
                    tok->Release();
                    break;
                }
                tok->Release();
                ++index;
            }
            en->Release();
        }
    }
    cat->Release();
    return set;
}

bool SapiBackend::Speak(int voiceID, const std::wstring &text, int rate,
                        int volume, uint32_t utteranceID) {
    if (!Init()) {
        Playback::Failed(utteranceID, "EngineAllocationFailed");
        return false;
    }
    if (!SelectVoice(voiceID)) {
        Playback::Failed(utteranceID, "InternalError");
        return false;
    }

    m_voice->SetRate(std::clamp(rate, -10, 10));
    m_voice->SetVolume(static_cast<USHORT>(std::clamp(volume, 0, 100)));

    ULONG streamNum = 0;
    if (FAILED(m_voice->Speak(text.c_str(), SPF_ASYNC, &streamNum))) {
        Playback::Failed(utteranceID, "InternalError");
        return false;
    }
    std::lock_guard<std::mutex> lock(m_streamMx);
    m_streamToUtterance[streamNum] = utteranceID;
    return true;
}

void SapiBackend::Stop() {
    if (m_voice == nullptr)
        return;
    m_voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
    std::lock_guard<std::mutex> lock(m_streamMx);
    m_streamToUtterance.clear();
}

} // namespace VoiceChat::Tts
