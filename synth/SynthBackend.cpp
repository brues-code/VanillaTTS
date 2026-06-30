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

#include <espeak-ng/speak_lib.h>
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace VoiceChat::Tts {

namespace {

// espeak's synth callback is a context-free C function. Synthesis is
// serialized on the single worker thread, so a file-scope pointer to the
// current job's PCM buffer is sufficient (no concurrency).
std::vector<int16_t> *g_accum = nullptr;

int SynthCallback(short *wav, int numsamples, espeak_EVENT *) {
    if (g_accum != nullptr && wav != nullptr && numsamples > 0)
        g_accum->insert(g_accum->end(), wav, wav + numsamples);
    return 0; // continue synthesis
}

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

// Directory containing this DLL (and, beside it, espeak-ng-data/).
std::string ModuleDir() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ModuleDir), &self) &&
        GetModuleFileNameA(self, path, MAX_PATH) > 0) {
        char *slash = std::strrchr(path, '\\');
        if (slash != nullptr)
            slash[1] = '\0';
        return path;
    }
    return {};
}

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

    // espeak-ng-data ships beside this DLL; pass our own directory as the
    // path espeak searches for it. DONT_EXIT so a data/init failure never
    // calls exit() and takes down WoW.
    const std::string dir = ModuleDir();
    const int rate = espeak_Initialize(AUDIO_OUTPUT_SYNCHRONOUS, /*buflength*/ 0,
                                       dir.empty() ? nullptr : dir.c_str(),
                                       espeakINITIALIZE_DONT_EXIT);
    if (rate <= 0)
        return false; // engine unavailable → selector falls back
    m_sampleRate = rate;
    espeak_SetSynthCallback(&SynthCallback);

    // Enumerate voices once, here on the main thread, and cache them — so
    // Voices()/Speak() never touch espeak concurrently with the worker.
    m_cachedVoices.clear();
    m_voiceIds.clear();
    if (const espeak_VOICE **list = espeak_ListVoices(nullptr)) {
        for (int i = 0; list[i] != nullptr; ++i) {
            const espeak_VOICE *v = list[i];
            const char *name = (v->name != nullptr) ? v->name : "";
            const char *id = (v->identifier != nullptr) ? v->identifier : name;
            m_cachedVoices.push_back(VoiceEntry{i, Utf8ToWide(name)});
            m_voiceIds.emplace_back(id);
        }
    }

    m_worker = std::thread(&SynthBackend::WorkerLoop, this);
    m_inited = true;
    return true;
}

std::vector<VoiceEntry> SynthBackend::Voices() {
    return m_cachedVoices; // cached in Init; no espeak call (avoids races)
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
    // Don't espeak_Cancel here — espeak is not thread-safe and the worker may
    // be mid-Synth. The generation bump makes the worker discard the in-flight
    // result; waveOutReset aborts any audio already playing.
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

std::vector<int16_t> SynthBackend::Synthesize(const Job &job) {
    if (job.voiceID >= 0 && job.voiceID < static_cast<int>(m_voiceIds.size()))
        espeak_SetVoiceByName(m_voiceIds[job.voiceID].c_str());

    // Map our API ranges to espeak's. rate: SAPI-style -10..10 (0 = normal)
    // → words/min around espeakRATE_NORMAL; volume: 0..100 → espeak 0..200.
    espeak_SetParameter(espeakRATE,
                        std::clamp(espeakRATE_NORMAL + job.rate * 20,
                                   espeakRATE_MINIMUM, espeakRATE_MAXIMUM),
                        0);
    espeak_SetParameter(espeakVOLUME, std::clamp(job.volume, 0, 200), 0);

    const std::string utf8 = WideToUtf8(job.text);
    std::vector<int16_t> out;
    g_accum = &out;
    espeak_Synth(utf8.c_str(), utf8.size() + 1, /*position*/ 0, POS_CHARACTER,
                 /*end_position*/ 0, espeakCHARS_UTF8, nullptr, nullptr);
    espeak_Synchronize();
    g_accum = nullptr;
    return out;
}

bool SynthBackend::PlayPcm(const std::vector<int16_t> &pcm) {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = static_cast<DWORD>(m_sampleRate);
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
            // CALLBACK_EVENT signals on buffer completion (and Stop's
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
