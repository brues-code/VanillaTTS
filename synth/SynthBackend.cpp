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

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace VoiceChat::Tts {

namespace {
constexpr int kSampleRate = 16000; // Hz, mono, 16-bit — matches espeak-ng output
} // namespace

SynthBackend::SynthBackend(const SynthHost &host) : m_host(host) {}

SynthBackend::~SynthBackend() {
    {
        std::lock_guard<std::mutex> lock(m_jobMx);
        m_quit = true;
        m_jobs.clear();
        m_generation.fetch_add(1);
    }
    m_jobCv.notify_all();
    if (auto *h = static_cast<HWAVEOUT>(m_activeWaveOut.load()))
        waveOutReset(h);
    if (m_worker.joinable())
        m_worker.join();
}

bool SynthBackend::Init() {
    if (m_inited)
        return true;
    // (5b: espeak_Initialize here; return false if it fails.)
    m_worker = std::thread(&SynthBackend::WorkerLoop, this);
    m_inited = true;
    return true;
}

std::vector<VoiceEntry> SynthBackend::Voices() {
    // (5b: enumerate espeak-ng voices.)
    return {VoiceEntry{0, L"VanillaTTS Software (stub)"}};
}

bool SynthBackend::Speak(int voiceID, const std::wstring &text, int rate,
                         int volume, uint32_t utteranceID) {
    if (!Init()) {
        m_host.failed(utteranceID, "EngineAllocationFailed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_jobMx);
        m_jobs.push_back(Job{utteranceID, text, voiceID, rate, volume});
    }
    m_jobCv.notify_one();
    return true;
}

void SynthBackend::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_jobMx);
        m_jobs.clear();
        m_generation.fetch_add(1);
    }
    if (auto *h = static_cast<HWAVEOUT>(m_activeWaveOut.load()))
        waveOutReset(h);
}

void SynthBackend::Pump() {
    std::vector<uint32_t> started;
    std::vector<std::pair<uint32_t, bool>> finished;
    {
        std::lock_guard<std::mutex> lock(m_resultMx);
        started.swap(m_startedQ);
        finished.swap(m_finishedQ);
    }
    // Started before finished so a same-tick start+finish surfaces in order.
    for (uint32_t id : started)
        m_host.started(id);
    for (const auto &f : finished) {
        if (f.second)
            m_host.finished(f.first);
        else
            m_host.failed(f.first, "InternalError");
    }
}

void SynthBackend::QueueStarted(uint32_t id) {
    std::lock_guard<std::mutex> lock(m_resultMx);
    m_startedQ.push_back(id);
}

void SynthBackend::QueueFinished(uint32_t id, bool success) {
    std::lock_guard<std::mutex> lock(m_resultMx);
    m_finishedQ.emplace_back(id, success);
}

void SynthBackend::WorkerLoop() {
    for (;;) {
        Job job;
        uint32_t myGen;
        {
            std::unique_lock<std::mutex> lock(m_jobMx);
            m_jobCv.wait(lock, [&] { return m_quit || !m_jobs.empty(); });
            if (m_quit)
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            myGen = m_generation.load();
        }

        std::vector<int16_t> pcm = Synthesize(job);
        if (m_generation.load() != myGen)
            continue; // purged during synthesis — no events
        if (pcm.empty()) {
            QueueFinished(job.id, false);
            continue;
        }

        QueueStarted(job.id);
        const bool ok = PlayPcm(pcm);
        if (m_generation.load() != myGen)
            continue; // purged during playback — no PLAYBACK_FINISHED
        QueueFinished(job.id, ok);
    }
}

// Placeholder synthesis: a 220 Hz sine whose length tracks the text, scaled
// by volume. Proves the PCM→waveOut path end to end; replaced by espeak-ng
// in Step 5b.
std::vector<int16_t> SynthBackend::Synthesize(const Job &job) {
    const size_t chars = job.text.size();
    // ~60 ms per character, clamped to [0.2 s, 3 s].
    double seconds = static_cast<double>(chars) * 0.06;
    seconds = std::clamp(seconds, 0.2, 3.0);
    const size_t samples = static_cast<size_t>(seconds * kSampleRate);

    const double amp = std::clamp(job.volume, 0, 100) / 100.0 * 8000.0;
    const double freq = 220.0;
    std::vector<int16_t> pcm(samples);
    for (size_t i = 0; i < samples; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        pcm[i] = static_cast<int16_t>(amp * std::sin(2.0 * 3.14159265358979323846 * freq * t));
    }
    return pcm;
}

bool SynthBackend::PlayPcm(const std::vector<int16_t> &pcm) {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = kSampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (done == nullptr)
        return false;

    HWAVEOUT hwo = nullptr;
    if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, reinterpret_cast<DWORD_PTR>(done), 0,
                    CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        CloseHandle(done);
        return false;
    }
    m_activeWaveOut.store(hwo);

    WAVEHDR hdr = {};
    hdr.lpData = reinterpret_cast<LPSTR>(const_cast<int16_t *>(pcm.data()));
    hdr.dwBufferLength = static_cast<DWORD>(pcm.size() * sizeof(int16_t));

    bool ok = false;
    if (waveOutPrepareHeader(hwo, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
        if (waveOutWrite(hwo, &hdr, sizeof hdr) == MMSYSERR_NOERROR) {
            // CALLBACK_EVENT signals on each buffer completion (and Stop's
            // waveOutReset). Wait until the buffer is flagged done.
            while ((hdr.dwFlags & WHDR_DONE) == 0)
                WaitForSingleObject(done, INFINITE);
            ok = true;
        }
        while (waveOutUnprepareHeader(hwo, &hdr, sizeof hdr) == WAVERR_STILLPLAYING)
            Sleep(1);
    }

    m_activeWaveOut.store(nullptr);
    waveOutClose(hwo);
    CloseHandle(done);
    return ok;
}

} // namespace VoiceChat::Tts
