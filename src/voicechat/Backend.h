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

#include <cstdint>
#include <string>
#include <vector>

namespace VoiceChat::Tts {

// One enumerated voice. `voiceID` is the backend's enumeration index (the
// value addons pass to SpeakText); `name` is the human-readable description.
struct VoiceEntry {
    int voiceID;
    std::wstring name;
};

// A speech engine. Selected at runtime — SAPI on native Windows, the Flite
// software synth under Wine (where SAPI is absent/voiceless). All methods
// are called on the main (game) thread.
struct ITtsBackend {
    // Probe + initialize. Returns false if the engine is unavailable on this
    // system (e.g. SAPI with no installed voices) — the selector then tries
    // the next backend.
    virtual bool Init() = 0;

    // The voices this backend offers, in enumeration order.
    virtual std::vector<VoiceEntry> Voices() = 0;

    // Begin speaking `text` with the voice at index `voiceID`. `utteranceID`
    // correlates the lifecycle events the backend reports via Playback::
    // below. Returns false if synthesis/playback couldn't start — in which
    // case the backend has ALREADY fired Playback::Failed (so the caller
    // must not fire it again).
    virtual bool Speak(int voiceID, const std::wstring &text, int rate,
                       int volume, uint32_t utteranceID) = 0;

    // Stop any in-progress / queued playback. No PLAYBACK_FINISHED is fired
    // for purged utterances (matches Blizzard's StopSpeakingText).
    virtual void Stop() = 0;

    virtual ~ITtsBackend() {}
};

// Lifecycle callbacks a backend invokes to drive the Lua event surface
// (VOICE_CHAT_TTS_PLAYBACK_*). Implemented by the module (Tts.cpp), which
// owns the utteranceID → destination correlation and the engine event
// dispatch. Must be called on the main (game) thread — Lua is not
// thread-safe, so a worker thread must marshal back first (e.g. via the
// WorldTick drain the Flite backend uses).
namespace Playback {
void Started(uint32_t utteranceID);
void Finished(uint32_t utteranceID);
void Failed(uint32_t utteranceID, const char *status);
} // namespace Playback

} // namespace VoiceChat::Tts
