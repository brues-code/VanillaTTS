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

// Text-to-speech — modern WoW's C_VoiceChat / C_TTSSettings Lua surface,
// backed by a pluggable speech engine (ITtsBackend). The module owns the
// API-facing concerns; the backend owns the actual synthesis/playback:
//   - SAPI on native Windows (SapiBackend)
//   - Flite software synth under Wine (loaded lazily as VanillaTTS_synth.dll)
//
// The integration seams stay project-native:
//   - namespaces  → Game::Lua::RegisterTableFunction
//   - events      → Event::Custom (AutoReserve + Fire)
//   - settings    → CVar::Factory (ttsVoice / ttsSpeed / ttsVolume / ttsEngine,
//                   persisted to Config.wtf, clamped via change callbacks)
//
// Lua surface:
//   C_VoiceChat
//     GetTtsVoices()            -> { {voiceID=, name=}, ... }
//     GetRemoteTtsVoices()      -> same (vanilla has no real voice chat)
//     SpeakText(voiceID, text[, destination[, rate[, volume]]])
//     StopSpeakingText()
//   C_TTSSettings
//     GetSpeechRate() / GetSpeechVolume() / GetSpeechVoiceID()
//     GetVoiceOptionName()
//     SetDefaultSettings()
//     SetSpeechRate(v) / SetSpeechVolume(v)
//     SetVoiceOption(id) / SetVoiceOptionByName(name)
//     RefreshVoices()
//
// Events: VOICE_CHAT_TTS_PLAYBACK_STARTED/FINISHED/FAILED,
//         VOICE_CHAT_TTS_SPEAK_TEXT_UPDATE (reserved, unused),
//         VOICE_CHAT_TTS_VOICES_UPDATE.

#include "Game.h"
#include "cvar/Factory.h"
#include "event/Custom.h"
#include "tick/WorldTick.h"
#include "voicechat/Backend.h"
#include "voicechat/SapiBackend.h"
#include "voicechat/SynthAbi.h"

#include <windows.h> // WideCharToMultiByte / MultiByteToWideChar, LoadLibrary

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VoiceChat::Tts {

namespace {

// ---------------------------------------------------------------------------
// Event names + reservations
// ---------------------------------------------------------------------------
constexpr const char *kEvtPlaybackFailed   = "VOICE_CHAT_TTS_PLAYBACK_FAILED";
constexpr const char *kEvtPlaybackFinished = "VOICE_CHAT_TTS_PLAYBACK_FINISHED";
constexpr const char *kEvtPlaybackStarted  = "VOICE_CHAT_TTS_PLAYBACK_STARTED";
constexpr const char *kEvtSpeakTextUpdate  = "VOICE_CHAT_TTS_SPEAK_TEXT_UPDATE";
constexpr const char *kEvtVoicesUpdate     = "VOICE_CHAT_TTS_VOICES_UPDATE";

const Event::Custom::AutoReserve _r1{kEvtPlaybackFailed};
const Event::Custom::AutoReserve _r2{kEvtPlaybackFinished};
const Event::Custom::AutoReserve _r3{kEvtPlaybackStarted};
const Event::Custom::AutoReserve _r4{kEvtSpeakTextUpdate};
const Event::Custom::AutoReserve _r5{kEvtVoicesUpdate};

// ---------------------------------------------------------------------------
// CVar names + handles (registered in RegisterLuaFunctions)
// ---------------------------------------------------------------------------
constexpr const char *kCVarVoice  = "ttsVoice";
constexpr const char *kCVarSpeed  = "ttsSpeed";
constexpr const char *kCVarVolume = "ttsVolume";
constexpr const char *kCVarEngine = "ttsEngine"; // "auto" / "sapi" / "flite"

CVar::Factory::Handle g_cvarVoice  = nullptr;
CVar::Factory::Handle g_cvarSpeed  = nullptr;
CVar::Factory::Handle g_cvarVolume = nullptr;
CVar::Factory::Handle g_cvarEngine = nullptr;

// Destination constants (kept for API parity; no special handling — the
// backends queue FIFO either way).
enum : int {
    DEST_LOCAL_PLAYBACK        = 1,
    DEST_QUEUED_LOCAL_PLAYBACK = 4,
};
int ClampDestination(int d) {
    return d == DEST_QUEUED_LOCAL_PLAYBACK ? DEST_QUEUED_LOCAL_PLAYBACK
                                           : DEST_LOCAL_PLAYBACK;
}

// ---------------------------------------------------------------------------
// Utterance lifecycle bookkeeping (backend-agnostic). The backend reports
// progress by utteranceID via Playback::; we map that to the destination the
// Lua caller requested and dedup the STARTED edge.
// ---------------------------------------------------------------------------
int g_nextUtteranceID = 1;
int NextUtteranceID() {
    if (g_nextUtteranceID >= 0x7fffffff)
        g_nextUtteranceID = 1;
    return g_nextUtteranceID++;
}

struct UtteranceMeta {
    int destination;
    bool startedEmitted;
};
std::mutex g_uttMx;
std::unordered_map<uint32_t, UtteranceMeta> g_uttMap;

std::vector<VoiceEntry> g_cachedVoices;

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
SapiBackend g_sapi;
ITtsBackend *g_backend = nullptr;

// Lazily load VanillaTTS_synth.dll (the espeak-ng software synth) from beside
// this DLL and adopt its backend. Cached after the first attempt so a missing
// synth DLL isn't retried on every selection. The synth backend reports
// playback lifecycle through SynthHost → Playback::, the same path SAPI uses.
ITtsBackend *LoadSynthBackend() {
    static bool tried = false;
    static ITtsBackend *cached = nullptr;
    if (tried)
        return cached;
    tried = true;

    // Resolve the synth DLL next to this (core) DLL: LoadLibrary's default
    // search starts at the EXE dir, not our dll_local, so build an explicit
    // path from our own module location.
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LoadSynthBackend), &self) &&
        GetModuleFileNameA(self, path, MAX_PATH) > 0) {
        char *slash = std::strrchr(path, '\\');
        if (slash != nullptr)
            slash[1] = '\0';
    } else {
        path[0] = '\0';
    }
    const std::string dll = std::string(path) + "VanillaTTS_synth.dll";

    HMODULE mod = LoadLibraryA(dll.c_str());
    if (mod == nullptr)
        mod = LoadLibraryA("VanillaTTS_synth.dll"); // fall back to default search
    if (mod == nullptr)
        return nullptr;

    auto create =
        reinterpret_cast<CreateBackendFn>(GetProcAddress(mod, kCreateBackendExport));
    if (create == nullptr)
        return nullptr;

    static const SynthHost host{&Playback::Started, &Playback::Finished,
                                &Playback::Failed};
    cached = create(&host);
    return cached;
}

// Pick a backend honoring the ttsEngine cvar. "auto" prefers SAPI and falls
// through to the software synth when SAPI is unavailable/voiceless (the Wine
// case); "sapi"/"espeak" force a specific engine, with a fallback so TTS
// still works if the forced engine can't initialize.
// Select a backend for an explicit engine mode. Taking the mode as a
// parameter (rather than always reading the cvar) is load-bearing for the
// change callback: when ttsEngine's callback fires, the engine has NOT yet
// stored the new value, so reading the cvar there would see the OLD engine.
// The callback passes the incoming value in directly.
ITtsBackend *SelectBackendFor(const std::string &mode) {
    if (mode == "sapi" || mode == "auto") {
        if (g_sapi.Init())
            return &g_sapi;
    }
    if (mode == "espeak" || mode == "auto") {
        ITtsBackend *synth = LoadSynthBackend();
        if (synth != nullptr && synth->Init())
            return synth;
    }
    // Forced engine couldn't initialize — fall back so TTS still functions.
    if (mode == "espeak") {
        if (g_sapi.Init())
            return &g_sapi;
    } else if (mode == "sapi") {
        ITtsBackend *synth = LoadSynthBackend();
        if (synth != nullptr && synth->Init())
            return synth;
    }
    return nullptr;
}

ITtsBackend *SelectBackend() {
    const char *raw = CVar::Factory::GetString(g_cvarEngine);
    return SelectBackendFor((raw != nullptr && *raw != '\0') ? raw : "auto");
}

ITtsBackend *ActiveBackend() {
    if (g_backend == nullptr)
        g_backend = SelectBackend();
    return g_backend;
}

// Subscribed to the engine's per-frame WorldTick (installed in DllMain). Lets
// the active backend deliver off-thread playback events on the main thread.
// Reads g_backend directly (not ActiveBackend) so an unused TTS system never
// forces backend selection from the tick.
void PumpBackend() {
    if (g_backend != nullptr)
        g_backend->Pump();
}
const Tick::WorldTick::AutoSubscribe _tickSub{&PumpBackend};

// ---------------------------------------------------------------------------
// UTF-8 / wide conversion
// ---------------------------------------------------------------------------
std::string WideToUtf8(const std::wstring &w) {
    if (w.empty())
        return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1)
        return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], need - 1, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const char *s) {
    if (s == nullptr || *s == '\0')
        return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (need <= 1)
        return {};
    std::wstring out(static_cast<size_t>(need - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], need);
    return out;
}

// ---------------------------------------------------------------------------
// Event fire helpers (engine dispatcher via Event::Custom)
// ---------------------------------------------------------------------------
void FireVoicesUpdate() {
    Event::Custom::Fire(Event::Custom::Lookup(kEvtVoicesUpdate), "");
}
void FirePlaybackStarted(int numConsumers, int utteranceID, int durationMS, int dest) {
    Event::Custom::Fire(Event::Custom::Lookup(kEvtPlaybackStarted), "%d%d%d%d",
                        numConsumers, utteranceID, durationMS, dest);
}
void FirePlaybackFinished(int numConsumers, int utteranceID, int dest) {
    Event::Custom::Fire(Event::Custom::Lookup(kEvtPlaybackFinished), "%d%d%d",
                        numConsumers, utteranceID, dest);
}
void FirePlaybackFailed(const char *status, int utteranceID, int dest) {
    Event::Custom::Fire(Event::Custom::Lookup(kEvtPlaybackFailed), "%s%d%d",
                        status, utteranceID, dest);
}

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------
std::vector<VoiceEntry> BackendVoices() {
    ITtsBackend *b = ActiveBackend();
    return b != nullptr ? b->Voices() : std::vector<VoiceEntry>{};
}

int VoiceCount() {
    return static_cast<int>(BackendVoices().size());
}

void RefreshVoices() {
    auto fresh = BackendVoices();
    bool changed = fresh.size() != g_cachedVoices.size();
    if (!changed) {
        for (size_t i = 0; i < fresh.size(); ++i) {
            if (fresh[i].name != g_cachedVoices[i].name) {
                changed = true;
                break;
            }
        }
    }
    g_cachedVoices = std::move(fresh);
    if (changed)
        FireVoicesUpdate();
}

// ---------------------------------------------------------------------------
// Speak / stop (route through the active backend)
// ---------------------------------------------------------------------------
void SpeakText(int voiceID, const std::wstring &text, int destination, int rate,
               int volume) {
    const uint32_t utteranceID = static_cast<uint32_t>(NextUtteranceID());
    const int dest = ClampDestination(destination);

    {
        std::lock_guard<std::mutex> lock(g_uttMx);
        g_uttMap[utteranceID] = UtteranceMeta{dest, false};
    }

    ITtsBackend *b = ActiveBackend();
    if (b == nullptr) {
        // No engine available — fire FAILED ourselves (no backend to do it).
        Playback::Failed(utteranceID, "EngineAllocationFailed");
        return;
    }
    // On failure the backend fires Playback::Failed itself (which clears the
    // utterance), so there's nothing more to do here.
    b->Speak(voiceID, text, rate, volume, utteranceID);
}

void StopAll() {
    ITtsBackend *b = ActiveBackend();
    if (b != nullptr)
        b->Stop();
    // No PLAYBACK_FINISHED for purged utterances (matches StopSpeakingText).
    std::lock_guard<std::mutex> lock(g_uttMx);
    g_uttMap.clear();
}

// ---------------------------------------------------------------------------
// Lua helpers
// ---------------------------------------------------------------------------
// Push a { {voiceID=, name=}, ... } array built from g_cachedVoices.
void PushVoicesTable(void *L) {
    Game::Lua::NewTable(L); // outer array at top
    for (size_t i = 0; i < g_cachedVoices.size(); ++i) {
        Game::Lua::PushNumber(L, static_cast<double>(i + 1)); // 1-based array key
        Game::Lua::NewTable(L);                               // sub-table
        Game::Lua::SetFieldNumber(L, "voiceID", g_cachedVoices[i].voiceID);
        Game::Lua::SetFieldString(L, "name",
                                  WideToUtf8(g_cachedVoices[i].name).c_str());
        // stack: [outer, key, sub] → outer[key] = sub
        Game::Lua::RawSet(L, -3);
    }
}

int OptInt(void *L, int idx, int fallback) {
    return Game::Lua::IsNumber(L, idx)
               ? static_cast<int>(Game::Lua::ToNumber(L, idx))
               : fallback;
}

// ---------------------------------------------------------------------------
// C_VoiceChat
// ---------------------------------------------------------------------------
int __fastcall Script_GetTtsVoices(void *L) {
    RefreshVoices();
    PushVoicesTable(L);
    return 1;
}

int __fastcall Script_GetRemoteTtsVoices(void *L) {
    RefreshVoices();
    PushVoicesTable(L);
    return 1;
}

int __fastcall Script_SpeakText(void *L) {
    if (!Game::Lua::IsNumber(L, 1) || !Game::Lua::IsString(L, 2)) {
        Game::Lua::Error(L, "Usage: C_VoiceChat.SpeakText(voiceID, text "
                            "[, destination, rate, volume])");
        return 0;
    }
    const int voiceID = static_cast<int>(Game::Lua::ToNumber(L, 1));
    const std::wstring text = Utf8ToWide(Game::Lua::ToString(L, 2));
    const int dest = OptInt(L, 3, DEST_LOCAL_PLAYBACK);
    const int rate = OptInt(L, 4, 0);
    const int volume = OptInt(L, 5, 100);
    SpeakText(voiceID, text, dest, rate, volume);
    return 0;
}

int __fastcall Script_StopSpeakingText(void *) {
    StopAll();
    return 0;
}

// ---------------------------------------------------------------------------
// C_TTSSettings
// ---------------------------------------------------------------------------
int __fastcall Script_GetSpeechRate(void *L) {
    Game::Lua::PushNumber(L, CVar::Factory::GetInt(g_cvarSpeed, 0));
    return 1;
}
int __fastcall Script_GetSpeechVolume(void *L) {
    Game::Lua::PushNumber(L, CVar::Factory::GetInt(g_cvarVolume, 100));
    return 1;
}
int __fastcall Script_GetSpeechVoiceID(void *L) {
    Game::Lua::PushNumber(L, CVar::Factory::GetInt(g_cvarVoice, 0));
    return 1;
}
int __fastcall Script_GetVoiceOptionName(void *L) {
    const int id = CVar::Factory::GetInt(g_cvarVoice, 0);
    auto voices = BackendVoices();
    if (id >= 0 && id < static_cast<int>(voices.size()))
        Game::Lua::PushString(L, WideToUtf8(voices[id].name).c_str());
    else
        Game::Lua::PushString(L, "");
    return 1;
}

int __fastcall Script_SetSpeechRate(void *L) {
    if (Game::Lua::IsNumber(L, 1))
        CVar::Factory::SetInt(g_cvarSpeed, static_cast<int>(Game::Lua::ToNumber(L, 1)));
    return 0;
}
int __fastcall Script_SetSpeechVolume(void *L) {
    if (Game::Lua::IsNumber(L, 1))
        CVar::Factory::SetInt(g_cvarVolume, static_cast<int>(Game::Lua::ToNumber(L, 1)));
    return 0;
}
int __fastcall Script_SetVoiceOption(void *L) {
    if (Game::Lua::IsNumber(L, 1))
        CVar::Factory::SetInt(g_cvarVoice, static_cast<int>(Game::Lua::ToNumber(L, 1)));
    return 0;
}
int __fastcall Script_SetVoiceOptionByName(void *L) {
    if (!Game::Lua::IsString(L, 1))
        return 0;
    const std::wstring want = Utf8ToWide(Game::Lua::ToString(L, 1));
    auto voices = BackendVoices();
    for (const auto &v : voices) {
        if (v.name.size() == want.size()) {
            bool eq = true;
            for (size_t i = 0; i < want.size(); ++i) {
                if (towlower(v.name[i]) != towlower(want[i])) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                CVar::Factory::SetInt(g_cvarVoice, v.voiceID);
                break;
            }
        }
    }
    return 0;
}
int __fastcall Script_SetDefaultSettings(void *) {
    const int maxVoice = VoiceCount();
    CVar::Factory::SetInt(g_cvarVoice, maxVoice > 1 ? 1 : 0);
    CVar::Factory::SetInt(g_cvarSpeed, 0);
    CVar::Factory::SetInt(g_cvarVolume, 100);
    FireVoicesUpdate();
    return 0;
}
int __fastcall Script_RefreshVoices(void *) {
    RefreshVoices();
    return 0;
}

// ---------------------------------------------------------------------------
// CVar clamp callbacks (re-entrant SetString to clamp; reject out-of-range)
// ---------------------------------------------------------------------------
int __fastcall ClampRange(CVar::Factory::Handle cvar, const char *, const char *next,
                          int lo, int hi) {
    if (next == nullptr)
        return 1;
    const int v = std::atoi(next);
    const int c = std::clamp(v, lo, hi);
    if (c != v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", c);
        CVar::Factory::SetString(cvar, buf);
        return 0;
    }
    return 1;
}
int __fastcall OnSpeedChanged(CVar::Factory::Handle c, const char *p, const char *n, void *) {
    return ClampRange(c, p, n, -10, 10);
}
int __fastcall OnVolumeChanged(CVar::Factory::Handle c, const char *p, const char *n, void *) {
    return ClampRange(c, p, n, 0, 100);
}
int __fastcall OnVoiceChanged(CVar::Factory::Handle c, const char *p, const char *n, void *) {
    const int maxIdx = VoiceCount() - 1;
    return ClampRange(c, p, n, 0, maxIdx < 0 ? 0 : maxIdx);
}
// ttsEngine accepts only "auto" / "sapi" / "espeak". Anything else is clamped
// to "auto" (re-entrant SetString, reject the bad input). On a valid change
// we drop the cached backend so the next use re-selects, and refresh the
// voice list (the active backend's voices differ).
int __fastcall OnEngineChanged(CVar::Factory::Handle c, const char *, const char *next, void *) {
    if (next == nullptr)
        return 1;
    if (std::strcmp(next, "auto") == 0 || std::strcmp(next, "sapi") == 0 ||
        std::strcmp(next, "espeak") == 0) {
        // Select from the incoming value — the cvar still holds the OLD value
        // until this callback accepts the change, so SelectBackend() (which
        // reads the cvar) would pick the wrong engine.
        g_backend = SelectBackendFor(next);
        RefreshVoices();
        return 1;
    }
    CVar::Factory::SetString(c, "auto");
    return 0;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void RegisterLuaFunctions() {
    // Engine first: the other cvars' callbacks consult it during their own
    // registration-time fire to pick the active backend for clamping.
    g_cvarEngine = CVar::Factory::Register(kCVarEngine, "auto", 0, &OnEngineChanged);
    g_cvarVoice  = CVar::Factory::Register(kCVarVoice,  "0",   0, &OnVoiceChanged);
    g_cvarSpeed  = CVar::Factory::Register(kCVarSpeed,  "0",   0, &OnSpeedChanged);
    g_cvarVolume = CVar::Factory::Register(kCVarVolume, "100", 0, &OnVolumeChanged);

    Game::Lua::RegisterTableFunction("C_VoiceChat", "GetTtsVoices", &Script_GetTtsVoices);
    Game::Lua::RegisterTableFunction("C_VoiceChat", "GetRemoteTtsVoices", &Script_GetRemoteTtsVoices);
    Game::Lua::RegisterTableFunction("C_VoiceChat", "SpeakText", &Script_SpeakText);
    Game::Lua::RegisterTableFunction("C_VoiceChat", "StopSpeakingText", &Script_StopSpeakingText);

    Game::Lua::RegisterTableFunction("C_TTSSettings", "GetSpeechRate", &Script_GetSpeechRate);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "GetSpeechVolume", &Script_GetSpeechVolume);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "GetSpeechVoiceID", &Script_GetSpeechVoiceID);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "GetVoiceOptionName", &Script_GetVoiceOptionName);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "SetDefaultSettings", &Script_SetDefaultSettings);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "SetSpeechRate", &Script_SetSpeechRate);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "SetSpeechVolume", &Script_SetSpeechVolume);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "SetVoiceOption", &Script_SetVoiceOption);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "SetVoiceOptionByName", &Script_SetVoiceOptionByName);
    Game::Lua::RegisterTableFunction("C_TTSSettings", "RefreshVoices", &Script_RefreshVoices);

    // The cvar change-callbacks above fire synchronously DURING their Register
    // calls — at which point g_cvarEngine isn't assigned yet, so any backend
    // selected then was chosen from a half-initialized state (a null engine
    // handle reads as "auto" → SAPI). Discard that stale selection and pick
    // once now, with all handles assigned and the persisted ttsEngine value
    // in place, so a saved "espeak" actually takes effect at boot.
    g_backend = nullptr;
    RefreshVoices();
}

const Game::ModuleAutoRegister _autoreg{&RegisterLuaFunctions};

} // namespace

// ---------------------------------------------------------------------------
// Playback lifecycle — called by the active backend (on the main thread) to
// drive the Lua event surface. Defined out of the anonymous namespace because
// they're declared in Backend.h with external linkage; they still reach the
// file-scope helpers/state above since this is the same TU.
// ---------------------------------------------------------------------------
namespace Playback {

void Started(uint32_t utteranceID) {
    int dest = DEST_LOCAL_PLAYBACK;
    bool fire = false;
    {
        std::lock_guard<std::mutex> lock(g_uttMx);
        auto it = g_uttMap.find(utteranceID);
        if (it != g_uttMap.end() && !it->second.startedEmitted) {
            dest = it->second.destination;
            it->second.startedEmitted = true;
            fire = true;
        }
    }
    if (fire)
        FirePlaybackStarted(1, static_cast<int>(utteranceID), /*durationMS*/ 0, dest);
}

void Finished(uint32_t utteranceID) {
    int dest = DEST_LOCAL_PLAYBACK;
    {
        std::lock_guard<std::mutex> lock(g_uttMx);
        auto it = g_uttMap.find(utteranceID);
        if (it != g_uttMap.end()) {
            dest = it->second.destination;
            g_uttMap.erase(it);
        }
    }
    FirePlaybackFinished(1, static_cast<int>(utteranceID), dest);
}

void Failed(uint32_t utteranceID, const char *status) {
    int dest = DEST_LOCAL_PLAYBACK;
    {
        std::lock_guard<std::mutex> lock(g_uttMx);
        auto it = g_uttMap.find(utteranceID);
        if (it != g_uttMap.end()) {
            dest = it->second.destination;
            g_uttMap.erase(it);
        }
    }
    FirePlaybackFailed(status, static_cast<int>(utteranceID), dest);
}

} // namespace Playback

} // namespace VoiceChat::Tts
