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
add_subdirectory(src/core)              # Phase 1+2: audio engine / control API
# add_subdirectory(src)   # Full source tree — Phase 1+ (gradual)

option(BUILD_TESTS "Build manual verification tools and Phase-0 POCs" OFF)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()

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

# --- KissFFT static library (Phase 3: HRTF convolution) ---
# BSD-3 / Unlicensed. Default scalar type is float (no FIXED_POINT / USE_SIMD).
add_library(kissfft STATIC
    third_party/kissfft/kiss_fft.c
    third_party/kissfft/kiss_fftr.c)
target_include_directories(kissfft PUBLIC third_party/kissfft)
if(MSVC)
    set_target_properties(kissfft PROPERTIES COMPILE_FLAGS "/wd4244 /wd4305 /wd4996")
endif()

# --- libmysofa static library (Phase 3: SOFA HRTF reader) ---
# Vendored from hoene/libmysofa 1.3.3. gunzip.c needs a zlib inflate API, which
# we satisfy with miniz (third_party/miniz/zlib.h -> miniz.h) instead of full zlib.
set(MYSOFA_SOURCES
    third_party/libmysofa/src/hrtf/cache.c
    third_party/libmysofa/src/hrtf/check.c
    third_party/libmysofa/src/hrtf/easy.c
    third_party/libmysofa/src/hrtf/interpolate.c
    third_party/libmysofa/src/hrtf/kdtree.c
    third_party/libmysofa/src/hrtf/lookup.c
    third_party/libmysofa/src/hrtf/loudness.c
    third_party/libmysofa/src/hrtf/minphase.c
    third_party/libmysofa/src/hrtf/neighbors.c
    third_party/libmysofa/src/hrtf/reader.c
    third_party/libmysofa/src/hrtf/resample.c
    third_party/libmysofa/src/hrtf/spherical.c
    third_party/libmysofa/src/hrtf/tools.c
    third_party/libmysofa/src/hdf/btree.c
    third_party/libmysofa/src/hdf/dataobject.c
    third_party/libmysofa/src/hdf/fractalhead.c
    third_party/libmysofa/src/hdf/gcol.c
    third_party/libmysofa/src/hdf/gunzip.c
    third_party/libmysofa/src/hdf/superblock.c
    third_party/libmysofa/src/resampler/speex_resampler.c
    third_party/miniz/miniz.c)
add_library(mysofa STATIC ${MYSOFA_SOURCES})
target_include_directories(mysofa PUBLIC third_party/libmysofa/src/hrtf)
target_include_directories(mysofa PRIVATE
    third_party/libmysofa/src/hdf
    third_party/libmysofa/src/resampler
    third_party/miniz)
if(MSVC)
    set_source_files_properties(${MYSOFA_SOURCES} PROPERTIES
        COMPILE_FLAGS "/wd4244 /wd4267 /wd4305 /wd4996 /wd4018 /wd4101 /wd4133")
endif()

# --- Production DSP library (Phase 2 + Phase 3 spatializer) ---
add_library(audio_dsp STATIC
    src/dsp/eq.cpp
    src/dsp/noise_suppressor.cpp
    src/dsp/spatializer.cpp)
target_include_directories(audio_dsp PUBLIC src/dsp third_party/rnnoise/include)
target_link_libraries(audio_dsp PUBLIC rnnoise kissfft mysofa)

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

message(STATUS "AnniAudio ${PROJECT_VERSION} — Phase 2 (Mixer + DSP)")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
