add_library(audio_core STATIC
    audio_utils.cpp
    AudioEngine.cpp
    AudioMixer.cpp
    midi_input.cpp
)
target_include_directories(audio_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(audio_core PUBLIC ole32 avrt winmm)

add_executable(test_routing test_routing.cpp)
target_link_libraries(test_routing PRIVATE audio_core)
