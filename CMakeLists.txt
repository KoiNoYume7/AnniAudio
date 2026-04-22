cmake_minimum_required(VERSION 3.20)
project(AnniAudio VERSION 0.1.0 LANGUAGES CXX C)

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
# add_subdirectory(src)   # Enabled from Phase 1 onwards
# add_subdirectory(tests) # Enabled from Phase 2 onwards

# --- Phase 0 POC targets (standalone, not part of the main build) ---
# Each POC is a self-contained executable built only when explicitly requested.
# They live in src/<module>/ alongside the eventual production code.
#
# Uncomment each target as you work through Phase 0:
#
# add_executable(poc_eq src/dsp/poc_eq.cpp)
# add_executable(poc_rnnoise src/noise/poc_rnnoise.cpp)
# add_executable(poc_hrtf src/spatial/poc_hrtf.cpp)
# add_executable(poc_wasapi src/core/poc_wasapi.cpp)
# add_executable(poc_nvidia src/noise/poc_nvidia.cpp)

message(STATUS "AnniAudio ${PROJECT_VERSION} — Phase 0 (Research)")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
