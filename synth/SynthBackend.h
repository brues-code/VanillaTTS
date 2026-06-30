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
#include "voicechat/SynthAbi.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace VoiceChat::Tts {

// Software-synthesis backend living in VanillaTTS_synth.dll. Synthesis +
// blocking playback run on a dedicated worker thread; playback lifecycle is
// queued and drained on the main (game) thread in Pump() (called from the
// core's WorldTick hook), where it invokes the SynthHost callbacks — so the
// Lua event surface is only ever touched on the main thread.
//
// Step 5a: Synthesize() emits a placeholder tone to prove the cross-DLL +
// threading + waveOut + event plumbing. Step 5b swaps in espeak-ng.
class SynthBackend : public ITtsBackend {
public:
    explicit SynthBackend(const SynthHost &host);
    ~SynthBackend() override;

    bool Init() override;
    std::vector<VoiceEntry> Voices() override;
    bool Speak(int voiceID, const std::wstring &text, int rate, int volume,
               uint32_t utteranceID) override;
    void Stop() override;
    void Pump() override;

private:
    struct Job {
        uint32_t id;
        std::wstring text;
        int voiceID;
        int rate;
        int volume;
    };

    void WorkerLoop();
    // Synthesize mono 16-bit PCM (at m_sampleRate) for `job` via espeak-ng.
    // Runs only on the worker thread (espeak is not reentrant).
    std::vector<int16_t> Synthesize(const Job &job);
    // Blocking playback of one PCM buffer; returns false on open/write error.
    // A concurrent Stop() (waveOutReset) ends the wait early and is detected
    // by the caller via the generation counter, not this return value.
    bool PlayPcm(const std::vector<int16_t> &pcm);

    void QueueStarted(uint32_t id);
    void QueueFinished(uint32_t id, bool success);

    SynthHost m_host;
    bool m_inited = false;
    int m_sampleRate = 22050; // set from espeak_Initialize

    // Voice list cached once in Init() (on the main thread) so Voices() and
    // Speak() never call espeak concurrently with the worker's synthesis.
    std::vector<VoiceEntry> m_cachedVoices;
    std::vector<std::string> m_voiceIds; // espeak identifier per voiceID

    std::thread m_worker;
    std::mutex m_jobMx;
    std::condition_variable m_jobCv;
    std::deque<Job> m_jobs;
    bool m_quit = false;

    // Drained on the main thread by Pump().
    std::mutex m_resultMx;
    std::vector<uint32_t> m_startedQ;
    std::vector<std::pair<uint32_t, bool>> m_finishedQ; // {id, success}

    // Bumped (under m_jobMx) on Stop(). A worker job captures the generation
    // when dequeued; if it changes mid-job the job was purged → no events.
    std::atomic<uint32_t> m_generation{0};
    // HWAVEOUT of the in-flight playback (void* to keep <windows.h> out of
    // the header), so Stop() can waveOutReset it. Null when idle.
    std::atomic<void *> m_activeWaveOut{nullptr};
};

} // namespace VoiceChat::Tts
