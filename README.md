# VanillaTTS

A standalone DLL injected into **World of Warcraft 1.12.1** (Turtle WoW / Octo
client, loaded by VanillaFixes) that adds modern WoW's text-to-speech Lua API:
`C_VoiceChat` + `C_TTSSettings`, the `VOICE_CHAT_TTS_*` events, and the
`ttsVoice` / `ttsSpeed` / `ttsVolume` CVars.

The speech engine sits behind a small backend interface so the same Lua API
works with two backends:

| Backend | Platform | Status |
|---------|----------|--------|
| **SAPI** (`VanillaTTS.dll`) | Native Windows | Stable — verified in-game |
| **espeak-ng** (`VanillaTTS_synth.dll`) | Wine/Linux (and native Windows) | **Experimental** |

> **⚠️ The espeak-ng software-synth backend is experimental.** It is built and
> has been verified to produce speech on **native Windows** (force it with
> `ttsEngine=espeak`), but it has **not yet been verified running under Wine**,
> which is its primary target. Use it at your own risk until Wine validation is
> done.

## Lua API

```lua
-- C_VoiceChat
C_VoiceChat.GetTtsVoices()            -- { {voiceID=, name=}, ... }
C_VoiceChat.GetRemoteTtsVoices()      -- same (vanilla has no real voice chat)
C_VoiceChat.SpeakText(voiceID, text [, destination [, rate [, volume]]])
C_VoiceChat.StopSpeakingText()

-- C_TTSSettings
C_TTSSettings.GetSpeechRate() / GetSpeechVolume() / GetSpeechVoiceID()
C_TTSSettings.GetVoiceOptionName()
C_TTSSettings.SetDefaultSettings()
C_TTSSettings.SetSpeechRate(v) / SetSpeechVolume(v)
C_TTSSettings.SetVoiceOption(id) / SetVoiceOptionByName(name)
C_TTSSettings.RefreshVoices()
```

Events: `VOICE_CHAT_TTS_PLAYBACK_STARTED` / `_FINISHED` / `_FAILED`,
`VOICE_CHAT_TTS_VOICES_UPDATE` (`VOICE_CHAT_TTS_SPEAK_TEXT_UPDATE` is reserved).

## CVars

Persisted to `WTF\Config.wtf`, clamped on change:

- `ttsVoice` — selected voice index (into the active backend's voice list)
- `ttsSpeed` — rate, `-10`..`10` (0 = normal)
- `ttsVolume` — `0`..`100`
- `ttsEngine` — `auto` (default) / `sapi` / `espeak`. `auto` prefers SAPI and
  falls back to espeak when SAPI is unavailable or voiceless (the Wine case).
  `sapi` / `espeak` force a specific backend.

## Building

32-bit only (WoW.exe is x86). Requires the submodules:

```sh
git submodule update --init --recursive
```

**espeak-ng** is built standalone first (its CTest/CPack/all-language-data
machinery hijacks the build dir if added as a subdirectory):

```sh
cmake -S external/espeak-ng -B external/espeak-ng/build -A Win32 \
  -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DBUILD_TESTING=OFF \
  -DUSE_ASYNC=OFF -DUSE_MBROLA=OFF -DUSE_LIBPCAUDIO=OFF \
  -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build external/espeak-ng/build --config Release
```

Then VanillaTTS:

```sh
cmake -B build -A Win32
cmake --build build --config Release
```

Outputs `build\Release\VanillaTTS.dll` and `build\Release\VanillaTTS_synth.dll`.

## Installing

1. Drop `VanillaTTS.dll` where VanillaFixes loads DLLs and add it to `dlls.txt`.
2. Put `VanillaTTS_synth.dll` **beside** `VanillaTTS.dll` (the core resolves it
   from its own directory). Do **not** add it to `dlls.txt` — it is loaded
   on demand, not injected.
3. Put the English-only `espeak-ng-data\` (produced by
   `scripts\trim-espeak-data.ps1`, ~1 MB) beside `VanillaTTS_synth.dll`.

Only the espeak path loads the synth DLL + data; pure-SAPI (Windows) users
never touch them.

## License

GPL v3. espeak-ng is GPL v3; SAPI is a Windows system component. See `COPYING`
in `external/espeak-ng` for its license.
