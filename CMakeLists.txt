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

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "src/*.h")

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# ole32: COM (CoInitializeEx / CoCreateInstance / CoTaskMemFree) for the SAPI
#        text-to-speech backend in src/voicechat/Tts.cpp. The SAPI GUIDs are
#        instantiated in that TU via <initguid.h>, so no sapi.lib is needed.
# winmm: waveOut* for the upcoming Flite PCM playback path (Step 6).
target_link_libraries(${PROJECT_NAME} PRIVATE minhook ole32 winmm)

target_include_directories(
  ${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
                          external/minhook/include)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "VanillaTTS")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MT")
set(CMAKE_CXX_FLAGS_RELEASE "/MT /O2 /Ob2 /DNDEBUG")
set(CMAKE_SHARED_LINKER_FLAGS
    "/OPT:REF /OPT:ICF /MERGE:.rdata=.text /MERGE:.pdata=.text /SECTION:.text,EWR /NODEFAULTLIB:MSVCRT"
)
