cmake_minimum_required(VERSION 3.10)

project(VanillaTTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# minhook's CMakeLists.txt declares `cmake_minimum_required(VERSION 3.0...3.5)`,
# which CMake 4.x flags as a deprecation warning every reconfigure. Bumping
# the minimum policy version here promotes its policy set to 3.10 without
# patching the vendored submodule.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
add_subdirectory(external/minhook)

# ---------------------------------------------------------------------------
# Core DLL (VanillaTTS.dll) — injected by VanillaFixes. Hooks + Lua API +
# SAPI backend. `src/` only; the synth DLL's sources live under `synth/`.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS "src/*.h")

add_library(VanillaTTS SHARED ${CORE_SOURCES} ${CORE_HEADERS})

# ole32: COM (CoInitializeEx / CoCreateInstance / CoTaskMemFree) for the SAPI
#        backend in src/voicechat/SapiBackend.cpp. The SAPI GUIDs are
#        instantiated in that TU via <initguid.h>, so no sapi.lib is needed.
target_link_libraries(VanillaTTS PRIVATE minhook ole32)

target_include_directories(
  VanillaTTS PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                     external/minhook/include)

set_target_properties(VanillaTTS PROPERTIES OUTPUT_NAME "VanillaTTS")

# ---------------------------------------------------------------------------
# Software-synth DLL (VanillaTTS_synth.dll) — lazily LoadLibrary'd by the core
# only on the Flite/Wine fallback path, so Windows users never load it. Holds
# the espeak-ng synth + PCM->waveOut playback. No MinHook; no engine glue.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SYNTH_SOURCES CONFIGURE_DEPENDS "synth/*.cpp")
file(GLOB_RECURSE SYNTH_HEADERS CONFIGURE_DEPENDS "synth/*.h")

add_library(VanillaTTS_synth SHARED ${SYNTH_SOURCES} ${SYNTH_HEADERS})

# winmm: waveOut* for PCM playback (works natively and under Wine).
target_link_libraries(VanillaTTS_synth PRIVATE winmm)

# Include `src/` for the shared ABI headers (voicechat/Backend.h, SynthAbi.h).
target_include_directories(
  VanillaTTS_synth PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/synth
                           ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(VanillaTTS_synth PROPERTIES OUTPUT_NAME "VanillaTTS_synth")

# ---------------------------------------------------------------------------
# Shared build/link settings (both DLLs)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MT")
set(CMAKE_CXX_FLAGS_RELEASE "/MT /O2 /Ob2 /DNDEBUG")
set(CMAKE_SHARED_LINKER_FLAGS
    "/OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text /SECTION:.text,EWR /NODEFAULTLIB:MSVCRT"
)
