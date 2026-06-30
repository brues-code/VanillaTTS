cmake_minimum_required(VERSION 3.10)

project(VanillaTTS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Honor CMAKE_MSVC_RUNTIME_LIBRARY so every target — including the C deps
# (minhook, espeak-ng) — links the static CRT (/MT), matching the DLLs.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# minhook's CMakeLists.txt declares `cmake_minimum_required(VERSION 3.0...3.5)`,
# which CMake 4.x flags as a deprecation warning every reconfigure. Bumping
# the minimum policy version here promotes its policy set to 3.10 without
# patching the vendored submodule.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
add_subdirectory(external/minhook)

# espeak-ng — the software-synth engine for the synth DLL. Built STANDALONE
# (not via add_subdirectory): its CTest/CPack/all-language-data machinery
# otherwise hijacks our build/ as the top-level project. Build it once with:
#   cmake -S external/espeak-ng -B external/espeak-ng/build -A Win32 \
#     -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DBUILD_TESTING=OFF \
#     -DUSE_ASYNC=OFF -DUSE_MBROLA=OFF -DUSE_LIBPCAUDIO=OFF \
#     -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
#   cmake --build external/espeak-ng/build --config Release
# That produces the static libs below + espeak-ng-data/ (ships beside the DLL).
set(ESPEAK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/espeak-ng)
set(ESPEAK_BUILD ${ESPEAK_DIR}/build)
set(ESPEAK_INCLUDE ${ESPEAK_DIR}/src/include)
set(ESPEAK_LIBS
    ${ESPEAK_BUILD}/src/libespeak-ng/Release/espeak-ng.lib
    ${ESPEAK_BUILD}/src/ucd-tools/Release/ucd.lib
    ${ESPEAK_BUILD}/src/speechPlayer/Release/speechPlayer.lib
    ${ESPEAK_BUILD}/sonic.dir/Release/sonic.lib)

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
# espeak-ng + its static deps (ucd, speechPlayer, sonic).
target_link_libraries(VanillaTTS_synth PRIVATE winmm ${ESPEAK_LIBS})

# espeak-ng's headers default to __declspec(dllimport) on Windows; when
# linking the STATIC lib, consumers must define LIBESPEAK_NG_EXPORT so
# ESPEAK_API resolves to the plain (non-__imp_) symbols. espeak's own static
# build sets this as an INTERFACE definition — we replicate it here.
target_compile_definitions(VanillaTTS_synth PRIVATE LIBESPEAK_NG_EXPORT=1)

# src/ for the shared ABI headers (voicechat/Backend.h, SynthAbi.h);
# ESPEAK_INCLUDE for <espeak-ng/speak_lib.h>.
target_include_directories(
  VanillaTTS_synth PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/synth
                           ${CMAKE_CURRENT_SOURCE_DIR}/src
                           ${ESPEAK_INCLUDE})

set_target_properties(VanillaTTS_synth PROPERTIES OUTPUT_NAME "VanillaTTS_synth")

# ---------------------------------------------------------------------------
# Shared build/link settings (both DLLs)
# ---------------------------------------------------------------------------
# Runtime (/MT) is set via CMAKE_MSVC_RUNTIME_LIBRARY above (CMP0091 NEW).
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /Ob2 /DNDEBUG")
set(CMAKE_SHARED_LINKER_FLAGS
    "/OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text /SECTION:.text,EWR /NODEFAULTLIB:MSVCRT"
)
