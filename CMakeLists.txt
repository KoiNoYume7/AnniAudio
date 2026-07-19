cmake_minimum_required(VERSION 3.20)

# Read version from VERSION file (single line, e.g. 0.2.0)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ANNI_VERSION_RAW LIMIT_COUNT 1)
string(STRIP "${ANNI_VERSION_RAW}" ANNI_VERSION)
project(AnniAudio VERSION ${ANNI_VERSION} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows 11 target — require at least Win10 1903 API surface (0x0A000007)
add_compile_definitions(
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Third-party include path (vendored header-only libs go here)
include_directories(${CMAKE_SOURCE_DIR}/third_party)
include_directories(${CMAKE_SOURCE_DIR}/include)

# --- Subdirectories ---
add_subdirectory(src/core)              # Phase 1: routing engine
# add_subdirectory(src)   # Full source tree — Phase 1+ (gradual)
# add_subdirectory(tests) # Enabled from Phase 2 onwards

# --- RNNoise static library (Phase 0 POC + production use) ---
set(RNNOISE_SOURCES
    third_party/rnnoise/src/denoise.c
    third_party/rnnoise/src/rnn.c
    third_party/rnnoise/src/pitch.c
    third_party/rnnoise/src/kiss_fft.c
    third_party/rnnoise/src/celt_lpc.c
    third_party/rnnoise/src/nnet.c
    third_party/rnnoise/src/nnet_default.c
    third_party/rnnoise/src/parse_lpcnet_weights.c
    third_party/rnnoise/src/rnnoise_data.c
    third_party/rnnoise/src/rnnoise_tables.c
)
add_library(rnnoise STATIC ${RNNOISE_SOURCES})
target_include_directories(rnnoise PUBLIC
    third_party/rnnoise/include
    third_party/rnnoise/src
)
target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
if(MSVC)
    set_source_files_properties(${RNNOISE_SOURCES} PROPERTIES
        COMPILE_FLAGS "/wd4244 /wd4267 /wd4305 /wd4996")
endif()

# --- Production DSP library (Phase 2) ---
add_library(audio_dsp STATIC src/dsp/eq.cpp src/dsp/noise_suppressor.cpp)
target_include_directories(audio_dsp PUBLIC src/dsp third_party/rnnoise/include)
target_link_libraries(audio_dsp PUBLIC rnnoise)

# --- Phase 0 POC targets (standalone, not part of the main build) ---
# Each POC is a self-contained executable built only when explicitly requested.
# They live in src/<module>/ alongside the eventual production code.
#
# Uncomment each target as you work through Phase 0:
#
add_executable(poc_eq src/dsp/poc_eq.cpp)
add_executable(poc_rnnoise src/noise/poc_rnnoise.cpp)
target_link_libraries(poc_rnnoise PRIVATE rnnoise)
# add_executable(poc_hrtf src/spatial/poc_hrtf.cpp)
add_executable(poc_wasapi src/core/poc_wasapi.cpp)
target_link_libraries(poc_wasapi PRIVATE ole32 avrt)

# Routing CLI — lists endpoints and routes audio between cables / physical devices
#
# MixerControlServer.cpp (the mixer's local HTTP+SSE control API, built on the
# vendored cpp-httplib) is compiled directly into this executable rather than
# into audio_core: it's route_cli-specific plumbing, and audio_core should stay
# a clean engine library with no networking dependency baked into it.
add_executable(route_cli src/core/route_cli.cpp src/core/MixerControlServer.cpp)
target_include_directories(route_cli PRIVATE src/core include src/dsp)
target_link_libraries(route_cli PRIVATE audio_core audio_dsp ole32 avrt ws2_32)
# add_executable(poc_nvidia src/noise/poc_nvidia.cpp)

# Manual verification tool for AudioMixer's live add/remove/rename path (not
# part of the default test suite; run by hand against live devices).
add_executable(test_mixer_live_edit src/core/test_mixer_live_edit.cpp)
target_link_libraries(test_mixer_live_edit PRIVATE audio_core)



message(STATUS "AnniAudio ${PROJECT_VERSION} — Phase 0 (Research)")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
